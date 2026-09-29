#include "identity.service.h"

#include <array>
#include <span>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include "modules/database/database.service.h"
#include "security.protocol.h"
namespace ultraterimal::server {
namespace {
std::string hex(std::span<const unsigned char> bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result += kHex[b >> 4];
        result += kHex[b & 15];
    }
    return result;
}
std::string passwordHash(std::string_view password, std::string_view salt, int iterations) {
    if (password.size() > 1024 || salt.size() != 64 || iterations < 600000 || iterations > 2000000) {
        throw std::invalid_argument("Invalid password hash parameters");
    }
    std::array<unsigned char, 32> bytes{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(bytes.size()), bytes.data()) != 1) {
        throw std::runtime_error("Password derivation failed");
    }
    return hex(bytes);
}
bool validUsername(std::string_view username) {
    return username.size() >= 3 && username.size() <= 64 && username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string_view::npos;
}
bool passwordMatches(const DatabaseRow& row, std::string_view password) {
    return !password.empty() && password.size() <= 1024 && constantEqual(row["password_hash"].as<std::string>(), passwordHash(password, row["password_salt"].as<std::string>(), row["password_iterations"].as<int>()));
}
void failedAttempt(DatabaseTransaction& tx, std::string_view id) {
    tx.exec("UPDATE users SET login_blocked_until=CASE WHEN failed_logins>=4 THEN now(6)+INTERVAL 5 MINUTE ELSE login_blocked_until END,failed_logins=CASE WHEN failed_logins>=4 THEN 0 ELSE failed_logins+1 END WHERE id=?", id);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.verify',false)", id);
}
bool factorMatches(DatabaseTransaction& tx, const DatabaseRow& row, std::string_view key, std::string_view code) {
    const auto encrypted = row["totp_secret"].as<std::string>();
    if (encrypted.empty()) {
        return code.empty();
    }
    const auto id = row["id"].as<std::string>();
    const auto step = verifyTotp(openSecret(key, "totp:" + id, encrypted), code, row["unix_time"].as<std::int64_t>(), row["totp_last_step"].as<std::int64_t>());
    if (step) {
        tx.exec("UPDATE users SET totp_last_step=? WHERE id=?", *step, id);
        return true;
    }
    if (code.size() != 32 || code.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return false;
    }
    return !tx.exec("DELETE FROM recovery_codes WHERE user_id=? AND code_hash=? RETURNING code_hash", id, tokenHash(code)).empty();
}
bool operationProofMatches(DatabaseTransaction& tx, const DatabaseRow& row, std::string_view key, std::string_view password, std::string_view code, const UserIdentity& user, std::string_view telegramProof) {
    if (!telegramProof.empty()) {
        if (!password.empty() || !code.empty() || telegramProof.size() != 32) {
            return false;
        }
        const auto proof = tx.exec("SELECT v.id FROM identity_telegram_verifications v JOIN telegram_accounts a ON a.user_id=v.user_id AND a.telegram_id=v.telegram_id JOIN telegram_settings b ON b.generation=v.bot_generation WHERE v.id=? AND v.user_id=? AND v.login_token_hash=? AND v.state='approved' AND v.expires_at>now(6) FOR UPDATE", telegramProof, user.id, user.loginHash);
        if (proof.empty()) {
            return false;
        }
        tx.exec("UPDATE identity_telegram_verifications SET state='consumed' WHERE id=?", telegramProof);
        return true;
    }
    // Exactly one proof: an empty or ambiguous request must never authenticate.
    if (!password.empty()) {
        return code.empty() && passwordMatches(row, password);
    }
    const auto encrypted = row["totp_secret"].as<std::string>();
    if (encrypted.empty() || code.size() != 6 || code.find_first_not_of("0123456789") != std::string_view::npos) {
        return false;
    }
    const auto id = row["id"].as<std::string>();
    const auto step = verifyTotp(openSecret(key, "totp:" + id, encrypted), code, row["unix_time"].as<std::int64_t>(), row["totp_last_step"].as<std::int64_t>());
    if (!step) {
        return false;
    }
    tx.exec("UPDATE users SET totp_last_step=? WHERE id=?", *step, id);
    return true;
}
DatabaseResult actorRows(DatabaseTransaction& tx, const UserIdentity& user) {
    return tx.exec("SELECT u.*,FLOOR(UNIX_TIMESTAMP(now(6))) AS unix_time,coalesce(login_blocked_until>now(6),false) AS blocked FROM users u WHERE u.id=? AND NOT u.disabled AND EXISTS(SELECT 1 FROM login_sessions s WHERE s.user_id=u.id AND s.token_hash=? AND s.expires_at>now(6)) FOR UPDATE", user.id, user.loginHash);
}
bool adminAllowed(DatabaseTransaction& tx, const UserIdentity& actor) {
    tx.exec("SELECT name FROM operation_locks WHERE name='accounts' FOR UPDATE");
    const auto rows = actorRows(tx, actor);
    return !rows.empty() && rows[0]["role"].as<std::string>() == "admin";
}
void invalidateLogins(DatabaseTransaction& tx, std::string_view id, std::string_view keep) {
    tx.exec("DELETE FROM login_sessions WHERE user_id=? AND token_hash<>?", id, keep);
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE (user_id=? OR owner_id=?) AND state IN ('pending','approved')", id, id);
}
} // namespace
std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Random generation failed");
    }
    return hex(bytes);
}
std::string tokenHash(std::string_view token) {
    std::array<unsigned char, 32> bytes{};
    unsigned size = 0;
    if (EVP_Digest(token.data(), token.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 || size != bytes.size()) {
        throw std::runtime_error("Token hashing failed");
    }
    return hex(bytes);
}
bool validToken(std::string_view token) { return token.size() == 64 && token.find_first_not_of("0123456789abcdef") == std::string_view::npos; }
void IdentityService::bootstrap(std::string_view username, std::string_view password) const {
    if (username.size() < 3 || username.size() > 64 || username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string_view::npos || password.size() < 12 || password.size() > 1024) {
        throw std::invalid_argument("Invalid bootstrap identity");
    }
    const auto salt = randomToken(), digest = passwordHash(password, salt, 600000), id = "u-" + randomToken();
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='bootstrap' FOR UPDATE");
    if (!tx.exec("SELECT id FROM users LIMIT 1").empty()) {
        throw std::runtime_error("Bootstrap already completed");
    }
    tx.exec("INSERT INTO users(id,username,password_salt,password_hash,password_iterations,role) VALUES(?,?,?,?,600000,'admin')", id, username, salt, digest);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.bootstrap',true)", id);
    tx.commit();
}
std::expected<LoginSession, LoginError> IdentityService::login(std::string_view username, std::string_view password, std::string_view code) const {
    if (!validUsername(username) || password.empty() || password.size() > 1024 || code.size() > 32) {
        return std::unexpected(LoginError::kInvalidCredentials);
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT *,FLOOR(UNIX_TIMESTAMP(now(6))) AS unix_time,coalesce(login_blocked_until>now(6),false) AS blocked FROM users WHERE username=? FOR UPDATE", username);
    if (rows.empty()) {
        (void)passwordHash(password, std::string(64, '0'), 600000);
        tx.exec("INSERT INTO audit_events(event_type,success) VALUES('identity.login',false)");
        tx.commit();
        return std::unexpected(LoginError::kInvalidCredentials);
    }
    const auto id = rows[0]["id"].as<std::string>();
    if (rows[0]["disabled"].as<bool>() || rows[0]["blocked"].as<bool>()) {
        return std::unexpected(LoginError::kInvalidCredentials);
    }
    const bool passwordValid = passwordMatches(rows[0], password);
    if (passwordValid && code.empty() && !rows[0]["totp_secret"].as<std::string>().empty()) {
        return std::unexpected(LoginError::kFactorRequired);
    }
    if (!passwordValid || !factorMatches(tx, rows[0], key_, code)) {
        failedAttempt(tx, id);
        tx.commit();
        return std::unexpected(LoginError::kInvalidCredentials);
    }
    LoginSession result{{id, rows[0]["username"].as<std::string>(), rows[0]["role"].as<std::string>(), {}}, randomToken()};
    result.user.loginHash = tokenHash(result.token);
    tx.exec("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", id);
    tx.exec("DELETE FROM login_sessions WHERE expires_at<=now(6) OR token_hash IN (SELECT token_hash FROM (SELECT token_hash FROM login_sessions WHERE user_id=? ORDER BY created_at DESC LIMIT 18446744073709551615 OFFSET 9) AS older)", id);
    tx.exec("INSERT INTO login_sessions(token_hash,user_id,expires_at) VALUES(?,?,now(6)+INTERVAL 8 HOUR)", result.user.loginHash, id);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.login',true)", id);
    tx.commit();
    return result;
}
std::optional<UserIdentity> IdentityService::authenticate(std::string_view token) const {
    if (!validToken(token)) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT u.id,u.username,u.role FROM users u JOIN login_sessions s ON s.user_id=u.id WHERE s.token_hash=? AND s.expires_at>now(6) AND NOT u.disabled", tokenHash(token));
    if (rows.empty()) {
        return std::nullopt;
    }
    return UserIdentity{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::string>(), tokenHash(token)};
}
void IdentityService::logout(std::string_view token) const {
    if (!validToken(token)) {
        return;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("DELETE FROM login_sessions WHERE token_hash=?", tokenHash(token));
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE login_token_hash=? AND state IN ('pending','approved')", tokenHash(token));
    tx.commit();
}
SecurityInfo IdentityService::security(std::string_view userId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT totp_secret<>'',(SELECT count(*) FROM recovery_codes WHERE user_id=u.id) FROM users u WHERE id=? AND NOT disabled", userId);
    if (rows.empty()) {
        return {};
    }
    return {rows[0][0].as<bool>(), rows[0][1].as<int>()};
}

std::optional<TelegramVerification> IdentityService::requestTelegramVerification(const UserIdentity& user, std::string_view generation) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto actor = actorRows(tx, user);
    if (actor.empty() || actor[0]["blocked"].as<bool>()) {
        return std::nullopt;
    }
    const auto binding = tx.exec("SELECT a.telegram_id FROM telegram_accounts a JOIN telegram_settings b ON b.generation=? WHERE a.user_id=?", generation, user.id);
    if (binding.empty() || !tx.exec("SELECT id FROM identity_telegram_verifications WHERE user_id=? AND created_at>now(6)-INTERVAL 60 SECOND", user.id).empty()) {
        return std::nullopt;
    }
    tx.exec("DELETE FROM identity_telegram_verifications WHERE expires_at<=now(6)");
    tx.exec("UPDATE identity_telegram_verifications SET state='failed' WHERE user_id=? AND state IN ('pending','approved')", user.id);
    TelegramVerification result{randomToken().substr(0, 32), binding[0][0].as<std::int64_t>()};
    tx.exec("INSERT INTO identity_telegram_verifications(id,user_id,login_token_hash,telegram_id,bot_generation,expires_at) VALUES(?,?,?,?,?,now(6)+INTERVAL 5 MINUTE)", result.id, user.id, user.loginHash, result.telegramId, generation);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.telegram.request',true)", user.id);
    tx.commit();
    return result;
}
std::optional<std::string> IdentityService::decideTelegramVerification(std::string_view generation, std::string_view id, std::int64_t telegramId, bool accept) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT v.user_id FROM identity_telegram_verifications v JOIN users u ON u.id=v.user_id AND NOT u.disabled JOIN login_sessions s ON s.token_hash=v.login_token_hash AND s.user_id=v.user_id AND s.expires_at>now(6) JOIN telegram_accounts a ON a.user_id=v.user_id AND a.telegram_id=v.telegram_id JOIN telegram_settings b ON b.generation=v.bot_generation WHERE v.id=? AND v.bot_generation=? AND v.telegram_id=? AND v.state='pending' AND v.expires_at>now(6) FOR UPDATE", id, generation, telegramId);
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto user = rows[0][0].as<std::string>();
    tx.exec("UPDATE identity_telegram_verifications SET state=? WHERE id=?", accept ? "approved" : "denied", id);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.telegram.decide',?)", user, accept);
    tx.commit();
    return user;
}
void IdentityService::failTelegramVerification(std::string_view id) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("UPDATE identity_telegram_verifications SET state='failed' WHERE id=? AND state IN ('pending','approved')", id);
    tx.commit();
}
bool IdentityService::verifyConnectionFactor(DatabaseTransaction& tx, const UserIdentity& user, std::string_view code) const {
    const auto rows = actorRows(tx, user);
    if (rows.empty() || rows[0]["blocked"].as<bool>()) {
        return false;
    }
    const bool ok = operationProofMatches(tx, rows[0], key_, {}, code, user, {});
    if (!ok) {
        failedAttempt(tx, user.id);
    } else {
        tx.exec("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    }
    return ok;
}
bool IdentityService::reauthenticate(const UserIdentity& user, std::string_view password, std::string_view code, std::string_view telegramProof) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = actorRows(tx, user);
    if (rows.empty() || rows[0]["blocked"].as<bool>()) {
        return false;
    }
    const bool ok = operationProofMatches(tx, rows[0], key_, password, code, user, telegramProof);
    if (!ok) {
        failedAttempt(tx, user.id);
    } else {
        tx.exec("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    }
    tx.commit();
    return ok;
}
std::optional<SecurityChange> IdentityService::changeSecurity(const UserIdentity& user, std::string_view action, std::string_view password, std::string_view code, std::string_view newPassword, std::string_view telegramProof) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = actorRows(tx, user);
    if (rows.empty() || rows[0]["blocked"].as<bool>()) {
        return std::nullopt;
    }
    const bool enabled = !rows[0]["totp_secret"].as<std::string>().empty();
    const bool validAction = (action == "begin" || action == "confirm") ? !enabled : action == "disable" || action == "recovery" ? enabled
                                                                                                                                 : action == "password" && newPassword.size() >= 12 && newPassword.size() <= 1024;
    if (!validAction || (action == "confirm" ? (!password.empty() || !telegramProof.empty()) : !operationProofMatches(tx, rows[0], key_, password, code, user, telegramProof))) {
        failedAttempt(tx, user.id);
        tx.commit();
        return std::nullopt;
    }
    SecurityChange result;
    if (action == "begin") {
        result.secret = newTotpSecret();
        tx.exec("UPDATE users SET totp_pending=?,totp_pending_expires_at=now(6)+INTERVAL 10 MINUTE WHERE id=?", sealSecret(key_, "pending:" + user.id, result.secret), user.id);
    } else if (action == "confirm") {
        const auto pending = rows[0]["totp_pending"].as<std::string>();
        const auto alive = tx.exec("SELECT 1 FROM users WHERE id=? AND totp_pending_expires_at>now(6)", user.id);
        if (pending.empty() || alive.empty()) {
            return std::nullopt;
        }
        const auto secret = openSecret(key_, "pending:" + user.id, pending);
        const auto step = verifyTotp(secret, code, rows[0]["unix_time"].as<std::int64_t>(), -1);
        if (!step) {
            failedAttempt(tx, user.id);
            tx.commit();
            return std::nullopt;
        }
        tx.exec("UPDATE users SET totp_secret=?,totp_last_step=?,totp_pending='',totp_pending_expires_at=NULL WHERE id=?", sealSecret(key_, "totp:" + user.id, secret), *step, user.id);
    } else if (action == "disable") {
        tx.exec("UPDATE users SET totp_secret='',totp_last_step=-1,totp_pending='',totp_pending_expires_at=NULL WHERE id=?", user.id);
        tx.exec("DELETE FROM recovery_codes WHERE user_id=?", user.id);
    } else if (action == "password") {
        const auto salt = randomToken();
        tx.exec("UPDATE users SET password_salt=?,password_hash=?,password_iterations=600000 WHERE id=?", salt, passwordHash(newPassword, salt, 600000), user.id);
    }
    if (action == "confirm" || action == "recovery") {
        tx.exec("DELETE FROM recovery_codes WHERE user_id=?", user.id);
        for (int i = 0; i < 10; ++i) {
            const auto recovery = randomToken().substr(0, 32);
            tx.exec("INSERT INTO recovery_codes(user_id,code_hash) VALUES(?,?)", user.id, tokenHash(recovery));
            result.recoveryCodes.push_back(recovery);
        }
    }
    if (action != "begin") {
        invalidateLogins(tx, user.id, user.loginHash);
    }
    tx.exec("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.security',true,?)", user.id, action);
    tx.commit();
    return result;
}
std::optional<std::vector<AccountInfo>> IdentityService::accounts(const UserIdentity& actor) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (!adminAllowed(tx, actor)) {
        return std::nullopt;
    }
    std::vector<AccountInfo> result;
    for (const auto& row : tx.exec("SELECT id,username,role,disabled,totp_secret<>'' FROM users ORDER BY created_at LIMIT 1000")) {
        result.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<bool>(), row[4].as<bool>()});
    }
    tx.commit();
    return result;
}
bool IdentityService::createAccount(const UserIdentity& actor, std::string_view username, std::string_view password, std::string_view role) const {
    if (!validUsername(username) || password.size() < 12 || password.size() > 1024 || (role != "admin" && role != "user")) {
        return false;
    }
    const auto salt = randomToken(), digest = passwordHash(password, salt, 600000), id = "u-" + randomToken();
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (!adminAllowed(tx, actor) || tx.exec("SELECT count(*) FROM users")[0][0].as<int>() >= 1000) {
        return false;
    }
    if (!tx.exec("SELECT id FROM users WHERE username=?", username).empty()) {
        return false;
    }
    tx.exec("INSERT INTO users(id,username,password_salt,password_hash,password_iterations,role) VALUES(?,?,?,?,600000,?)", id, username, salt, digest, role);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.create',true,?)", actor.id, id);
    tx.commit();
    return true;
}
bool IdentityService::updateAccount(const UserIdentity& actor, std::string_view id, std::string_view role, bool disabled, std::string_view password) const {
    if ((role != "admin" && role != "user") || (!password.empty() && (password.size() < 12 || password.size() > 1024))) {
        return false;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (!adminAllowed(tx, actor)) {
        return false;
    }
    const auto rows = tx.exec("SELECT role,disabled FROM users WHERE id=? FOR UPDATE", id);
    if (rows.empty()) {
        return false;
    }
    if (rows[0][0].as<std::string>() == "admin" && !rows[0][1].as<bool>() && (role != "admin" || disabled) && tx.exec("SELECT count(*) FROM users WHERE role='admin' AND NOT disabled")[0][0].as<int>() <= 1) {
        return false;
    }
    tx.exec("UPDATE users SET role=?,disabled=? WHERE id=?", role, disabled, id);
    if (!password.empty()) {
        const auto salt = randomToken();
        tx.exec("UPDATE users SET password_salt=?,password_hash=?,password_iterations=600000,failed_logins=0,login_blocked_until=NULL WHERE id=?", salt, passwordHash(password, salt, 600000), id);
    }
    invalidateLogins(tx, id, {});
    tx.exec("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.update',true,?)", actor.id, id);
    tx.commit();
    return true;
}
} // namespace ultraterimal::server
