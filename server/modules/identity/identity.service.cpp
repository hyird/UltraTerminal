#include "identity.service.h"

#include <array>
#include <binary_text_encoding.h>
#include <controller.h>
#include <crypto.h>
#include <span>
#include <stdexcept>

#include <openssl/evp.h>

#include "common/database.h"
#include "identity.entity.h"
#include "security.protocol.h"
namespace ultraterimal::server {
namespace {
struct DerivedPassword {
    std::string salt;
    int iterations = 0;
    std::string hash;
};
std::string passwordHash(std::string_view password, std::string_view salt, int iterations) {
    if (password.size() > 1024 || salt.size() != 64 || iterations < 600000 || iterations > 2000000) {
        throw std::invalid_argument("Invalid password hash parameters");
    }
    std::array<unsigned char, 32> bytes{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(bytes.size()), bytes.data()) != 1) {
        throw std::runtime_error("Password derivation failed");
    }
    return ruvia::encode_lower_hex(std::as_bytes(std::span(bytes)));
}
ruvia::task<std::string> derivePasswordHash(ruvia::context* context, std::string password, std::string salt, int iterations) {
    if (context) {
        co_return (co_await context->run_blocking([password = std::move(password), salt = std::move(salt), iterations] {
            return passwordHash(password, salt, iterations);
        })).take_value();
    }
    co_return passwordHash(password, salt, iterations);
}
bool validUsername(std::string_view username) {
    return username.size() >= 3 && username.size() <= 64 && username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == std::string_view::npos;
}
bool derivedPasswordMatches(const ruvia::db_row& row, const std::optional<DerivedPassword>& derived) {
    return derived && derived->salt == dbField<std::string>(row["password_salt"]) && derived->iterations == dbField<int>(row["password_iterations"]) && constantEqual(dbField<std::string>(row["password_hash"]), derived->hash);
}
ruvia::task<std::optional<DerivedPassword>> precomputeUserPassword(ruvia::db_executor db, ruvia::context* context, std::string userId, std::string password) {
    if (password.empty() || password.size() > 1024) {
        co_return std::nullopt;
    }
    const auto rows = co_await db.query("SELECT password_salt,password_iterations FROM users WHERE id=? AND NOT disabled", userId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto salt = dbField<std::string>(rows[0][0]);
    const auto iterations = dbField<int>(rows[0][1]);
    auto hash = co_await derivePasswordHash(context, std::move(password), salt, iterations);
    co_return DerivedPassword{salt, iterations, std::move(hash)};
}
ruvia::task<void> failedAttempt(ruvia::db_transaction& tx, std::string id) {
    co_await tx.execute("UPDATE users SET login_blocked_until=CASE WHEN failed_logins>=4 THEN UTC_TIMESTAMP(6)+INTERVAL 5 MINUTE ELSE login_blocked_until END,failed_logins=CASE WHEN failed_logins>=4 THEN 0 ELSE failed_logins+1 END WHERE id=?", id);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.verify',false)", id);
}
ruvia::task<bool> factorMatches(ruvia::db_transaction& tx, const ruvia::db_row& row, std::string key, std::string code) {
    const auto encrypted = dbField<std::string>(row["totp_secret"]);
    if (encrypted.empty()) {
        co_return code.empty();
    }
    const auto id = dbField<std::string>(row["id"]);
    const auto step = verifyTotp(openSecret(key, "totp:" + id, encrypted), code, dbField<std::int64_t>(row["unix_time"]), dbField<std::int64_t>(row["totp_last_step"]));
    if (step) {
        co_await tx.execute("UPDATE users SET totp_last_step=? WHERE id=?", *step, id);
        co_return true;
    }
    if (code.size() != 32 || code.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        co_return false;
    }
    const auto deleted = co_await tx.query("DELETE FROM recovery_codes WHERE user_id=? AND code_hash=? RETURNING code_hash", id, tokenHash(code));
    co_return !deleted.empty();
}
ruvia::task<bool> operationProofMatches(ruvia::db_transaction& tx, const ruvia::db_row& row, std::string key, std::string password, const std::optional<DerivedPassword>& derivedPassword, std::string code, UserIdentity user, std::string telegramProof) {
    if (!telegramProof.empty()) {
        if (!password.empty() || !code.empty() || telegramProof.size() != 32) {
            co_return false;
        }
        const auto proof = co_await tx.query("SELECT v.id FROM identity_telegram_verifications v JOIN telegram_accounts a ON a.user_id=v.user_id AND a.telegram_id=v.telegram_id JOIN telegram_settings b ON b.generation=v.bot_generation WHERE v.id=? AND v.user_id=? AND v.login_token_hash=? AND v.state='approved' AND v.expires_at>UTC_TIMESTAMP(6) FOR UPDATE", telegramProof, user.id, user.loginHash);
        if (proof.empty()) {
            co_return false;
        }
        IdentityTelegramVerificationEntity consumed;
        consumed.set<"state">("consumed");
        co_await tx.repository<IdentityTelegramVerificationEntity>().update(IdentityTelegramVerificationEntity::column<"id">() == telegramProof, consumed);
        co_return true;
    }
    // Exactly one proof: an empty or ambiguous request must never authenticate.
    if (!password.empty()) {
        co_return code.empty() && derivedPasswordMatches(row, derivedPassword);
    }
    const auto encrypted = dbField<std::string>(row["totp_secret"]);
    if (encrypted.empty() || code.size() != 6 || code.find_first_not_of("0123456789") != std::string_view::npos) {
        co_return false;
    }
    const auto id = dbField<std::string>(row["id"]);
    const auto step = verifyTotp(openSecret(key, "totp:" + id, encrypted), code, dbField<std::int64_t>(row["unix_time"]), dbField<std::int64_t>(row["totp_last_step"]));
    if (!step) {
        co_return false;
    }
    co_await tx.execute("UPDATE users SET totp_last_step=? WHERE id=?", *step, id);
    co_return true;
}
ruvia::task<ruvia::db_rows> actorRows(ruvia::db_transaction& tx, UserIdentity user) {
    co_return co_await tx.query("SELECT u.*,FLOOR(UNIX_TIMESTAMP()) AS unix_time,coalesce(login_blocked_until>UTC_TIMESTAMP(6),false) AS blocked FROM users u WHERE u.id=? AND NOT u.disabled AND EXISTS(SELECT 1 FROM login_sessions s WHERE s.user_id=u.id AND s.token_hash=? AND s.expires_at>UTC_TIMESTAMP(6)) FOR UPDATE", user.id, user.loginHash);
}
ruvia::task<bool> adminAllowed(ruvia::db_transaction& tx, UserIdentity actor) {
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='accounts' FOR UPDATE");
    const auto rows = co_await actorRows(tx, std::move(actor));
    co_return !rows.empty() && dbField<std::string>(rows[0]["role"]) == "admin";
}
ruvia::task<void> invalidateLogins(ruvia::db_transaction& tx, std::string id, std::string keep) {
    co_await tx.repository<LoginSessionEntity>().delete_by(LoginSessionEntity::column<"user_id">() == id && LoginSessionEntity::column<"token_hash">() != keep);
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE user_id=? AND state IN ('pending','approved')", id);
}
} // namespace
std::string randomToken() {
    std::array<std::byte, 32> bytes{};
    if (!ruvia::fill_random_bytes(bytes)) {
        throw std::runtime_error("Random generation failed");
    }
    return ruvia::encode_lower_hex(bytes);
}
std::string tokenHash(std::string_view token) {
    return ruvia::encode_lower_hex(ruvia::sha256_of(token));
}
bool validToken(std::string_view token) { return ruvia::is_lower_hex(token, 64); }
ruvia::task<void> IdentityService::bootstrap(ruvia::db_executor db, std::string username, std::string password) const {
    if (username.size() < 3 || username.size() > 64 || username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string_view::npos || password.size() < 12 || password.size() > 1024) {
        throw std::invalid_argument("Invalid bootstrap identity");
    }
    const auto salt = randomToken(), digest = passwordHash(password, salt, 600000), id = "u-" + randomToken();
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='bootstrap' FOR UPDATE");
    if (co_await tx.repository<UserEntity>().exists({})) {
        throw std::runtime_error("Bootstrap already completed");
    }
    co_await tx.execute("INSERT INTO users(id,username,password_salt,password_hash,password_iterations,role) VALUES(?,?,?,?,600000,'admin')", id, username, salt, digest);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.bootstrap',true)", id);
    co_await tx.commit();
}
ruvia::task<std::expected<LoginSession, LoginError>> IdentityService::login(ruvia::db_executor db, std::string username, std::string password, std::string code, ruvia::context* context) const {
    if (!validUsername(username) || password.empty() || password.size() > 1024 || code.size() > 32) {
        co_return std::unexpected(LoginError::kInvalidCredentials);
    }
    // Derive PBKDF2 outside any DB transaction and on the blocking pool to avoid
    // stalling the I/O event loop or holding row locks during CPU-bound hashing.
    std::optional<DerivedPassword> derived;
    const auto preview = co_await db.query("SELECT password_salt,password_iterations,disabled,coalesce(login_blocked_until>UTC_TIMESTAMP(6),false) AS blocked FROM users WHERE username=?", username);
    if (preview.empty()) {
        (void)co_await derivePasswordHash(context, password, std::string(64, '0'), 600000);
        co_await db.execute("INSERT INTO audit_events(event_type,success) VALUES('identity.login',false)");
        co_return std::unexpected(LoginError::kInvalidCredentials);
    }
    if (!dbField<bool>(preview[0]["disabled"]) && !dbField<bool>(preview[0]["blocked"])) {
        const auto salt = dbField<std::string>(preview[0]["password_salt"]);
        const auto iterations = dbField<int>(preview[0]["password_iterations"]);
        auto hash = co_await derivePasswordHash(context, password, salt, iterations);
        derived = DerivedPassword{salt, iterations, std::move(hash)};
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT *,FLOOR(UNIX_TIMESTAMP()) AS unix_time,coalesce(login_blocked_until>UTC_TIMESTAMP(6),false) AS blocked FROM users WHERE username=? FOR UPDATE", username);
    if (rows.empty()) {
        co_await tx.execute("INSERT INTO audit_events(event_type,success) VALUES('identity.login',false)");
        co_await tx.commit();
        co_return std::unexpected(LoginError::kInvalidCredentials);
    }
    const auto id = dbField<std::string>(rows[0]["id"]);
    if (dbField<bool>(rows[0]["disabled"]) || dbField<bool>(rows[0]["blocked"])) {
        co_return std::unexpected(LoginError::kInvalidCredentials);
    }
    const bool passwordValid = derivedPasswordMatches(rows[0], derived);
    if (passwordValid && code.empty() && !dbField<std::string>(rows[0]["totp_secret"]).empty()) {
        co_return std::unexpected(LoginError::kFactorRequired);
    }
    if (!passwordValid || !(co_await factorMatches(tx, rows[0], key_, code))) {
        co_await failedAttempt(tx, id);
        co_await tx.commit();
        co_return std::unexpected(LoginError::kInvalidCredentials);
    }
    LoginSession result{{id, dbField<std::string>(rows[0]["username"]), dbField<std::string>(rows[0]["role"]), {}}, randomToken()};
    result.user.loginHash = tokenHash(result.token);
    co_await tx.execute("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", id);
    co_await tx.execute("DELETE FROM login_sessions WHERE expires_at<=UTC_TIMESTAMP(6) OR token_hash IN (SELECT token_hash FROM (SELECT token_hash FROM login_sessions WHERE user_id=? ORDER BY created_at DESC LIMIT 18446744073709551615 OFFSET 9) AS older)", id);
    co_await tx.execute("INSERT INTO login_sessions(token_hash,user_id,expires_at) VALUES(?,?,UTC_TIMESTAMP(6)+INTERVAL 8 HOUR)", result.user.loginHash, id);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.login',true)", id);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<UserIdentity>> IdentityService::authenticate(ruvia::db_executor db, std::string token) const {
    if (!validToken(token)) {
        co_return std::nullopt;
    }
    const auto hash = tokenHash(token);
    const auto rows = co_await db.query("SELECT u.id,u.username,u.role FROM users u JOIN login_sessions s ON s.user_id=u.id WHERE s.token_hash=? AND s.expires_at>UTC_TIMESTAMP(6) AND NOT u.disabled", hash);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return UserIdentity{dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), dbField<std::string>(rows[0][2]), hash};
}
ruvia::task<void> IdentityService::logout(ruvia::db_executor db, std::string token) const {
    if (!validToken(token)) {
        co_return;
    }
    const auto hash = tokenHash(token);
    auto tx = co_await db.begin_transaction(kReadCommitted);
    co_await tx.repository<LoginSessionEntity>().delete_by(LoginSessionEntity::column<"token_hash">() == hash);
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE login_token_hash=? AND state IN ('pending','approved')", hash);
    co_await tx.commit();
}
ruvia::task<SecurityInfo> IdentityService::security(ruvia::db_executor db, std::string userId) const {
    const auto rows = co_await db.query("SELECT totp_secret<>'',(SELECT count(*) FROM recovery_codes WHERE user_id=u.id) FROM users u WHERE id=? AND NOT disabled", userId);
    if (rows.empty()) {
        co_return SecurityInfo{};
    }
    co_return SecurityInfo{dbField<bool>(rows[0][0]), dbField<int>(rows[0][1])};
}
ruvia::task<std::optional<TelegramVerification>> IdentityService::requestTelegramVerification(ruvia::db_executor db, UserIdentity user, std::string generation) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto actor = co_await actorRows(tx, user);
    if (actor.empty() || dbField<bool>(actor[0]["blocked"])) {
        co_return std::nullopt;
    }
    const auto binding = co_await tx.query("SELECT a.telegram_id FROM telegram_accounts a JOIN telegram_settings b ON b.generation=? WHERE a.user_id=?", generation, user.id);
    if (binding.empty()) {
        co_return std::nullopt;
    }
    const auto recent = co_await tx.query("SELECT id FROM identity_telegram_verifications WHERE user_id=? AND created_at>UTC_TIMESTAMP(6)-INTERVAL 60 SECOND", user.id);
    if (!recent.empty()) {
        co_return std::nullopt;
    }
    co_await tx.execute("DELETE FROM identity_telegram_verifications WHERE expires_at<=UTC_TIMESTAMP(6)");
    co_await tx.execute("UPDATE identity_telegram_verifications SET state='failed' WHERE user_id=? AND state IN ('pending','approved')", user.id);
    TelegramVerification result{randomToken().substr(0, 32), dbField<std::int64_t>(binding[0][0])};
    co_await tx.execute("INSERT INTO identity_telegram_verifications(id,user_id,login_token_hash,telegram_id,bot_generation,expires_at) VALUES(?,?,?,?,?,UTC_TIMESTAMP(6)+INTERVAL 5 MINUTE)", result.id, user.id, user.loginHash, result.telegramId, generation);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.telegram.request',true)", user.id);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<std::string>> IdentityService::decideTelegramVerification(ruvia::db_executor db, std::string generation, std::string id, std::int64_t telegramId, bool accept) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT v.user_id FROM identity_telegram_verifications v JOIN users u ON u.id=v.user_id AND NOT u.disabled JOIN login_sessions s ON s.token_hash=v.login_token_hash AND s.user_id=v.user_id AND s.expires_at>UTC_TIMESTAMP(6) JOIN telegram_accounts a ON a.user_id=v.user_id AND a.telegram_id=v.telegram_id JOIN telegram_settings b ON b.generation=v.bot_generation WHERE v.id=? AND v.bot_generation=? AND v.telegram_id=? AND v.state='pending' AND v.expires_at>UTC_TIMESTAMP(6) FOR UPDATE", id, generation, telegramId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto user = dbField<std::string>(rows[0][0]);
    IdentityTelegramVerificationEntity decided;
    decided.set<"state">(accept ? "approved" : "denied");
    co_await tx.repository<IdentityTelegramVerificationEntity>().update(IdentityTelegramVerificationEntity::column<"id">() == id, decided);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'identity.telegram.decide',?)", user, accept);
    co_await tx.commit();
    co_return user;
}
ruvia::task<void> IdentityService::failTelegramVerification(ruvia::db_executor db, std::string id) const {
    co_await db.execute("UPDATE identity_telegram_verifications SET state='failed' WHERE id=? AND state IN ('pending','approved')", id);
}
ruvia::task<bool> IdentityService::verifyConnectionFactor(ruvia::db_transaction& tx, UserIdentity user, std::string code) const {
    const auto rows = co_await actorRows(tx, user);
    if (rows.empty() || dbField<bool>(rows[0]["blocked"])) {
        co_return false;
    }
    const bool ok = co_await operationProofMatches(tx, rows[0], key_, {}, std::nullopt, code, user, {});
    if (!ok) {
        co_await failedAttempt(tx, user.id);
    } else {
        co_await tx.execute("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    }
    co_return ok;
}
ruvia::task<bool> IdentityService::reauthenticate(ruvia::db_executor db, UserIdentity user, std::string password, std::string code, std::string telegramProof, ruvia::context* context) const {
    std::optional<DerivedPassword> derived;
    if (!password.empty() && code.empty() && telegramProof.empty()) {
        derived = co_await precomputeUserPassword(db, context, user.id, password);
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await actorRows(tx, user);
    if (rows.empty() || dbField<bool>(rows[0]["blocked"])) {
        co_return false;
    }
    const bool ok = co_await operationProofMatches(tx, rows[0], key_, password, derived, code, user, telegramProof);
    if (!ok) {
        co_await failedAttempt(tx, user.id);
    } else {
        co_await tx.execute("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    }
    co_await tx.commit();
    co_return ok;
}
ruvia::task<std::optional<SecurityChange>> IdentityService::changeSecurity(ruvia::db_executor db, UserIdentity user, std::string action, std::string password, std::string code, std::string newPassword, std::string telegramProof, ruvia::context* context) const {
    std::optional<DerivedPassword> derived;
    if (action != "confirm" && !password.empty() && code.empty() && telegramProof.empty()) {
        derived = co_await precomputeUserPassword(db, context, user.id, password);
    }
    std::string newSalt;
    std::string newDigest;
    if (action == "password" && newPassword.size() >= 12 && newPassword.size() <= 1024) {
        newSalt = randomToken();
        newDigest = co_await derivePasswordHash(context, newPassword, newSalt, 600000);
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await actorRows(tx, user);
    if (rows.empty() || dbField<bool>(rows[0]["blocked"])) {
        co_return std::nullopt;
    }
    const bool enabled = !dbField<std::string>(rows[0]["totp_secret"]).empty();
    const bool validAction = (action == "begin" || action == "confirm") ? !enabled : action == "disable" || action == "recovery" ? enabled
                                                                                                                                 : action == "password" && newPassword.size() >= 12 && newPassword.size() <= 1024;
    const bool proofOk = validAction && (action == "confirm" ? (password.empty() && telegramProof.empty()) : co_await operationProofMatches(tx, rows[0], key_, password, derived, code, user, telegramProof));
    if (!proofOk) {
        co_await failedAttempt(tx, user.id);
        co_await tx.commit();
        co_return std::nullopt;
    }
    SecurityChange result;
    if (action == "begin") {
        result.secret = newTotpSecret();
        co_await tx.execute("UPDATE users SET totp_pending=?,totp_pending_expires_at=UTC_TIMESTAMP(6)+INTERVAL 10 MINUTE WHERE id=?", sealSecret(key_, "pending:" + user.id, result.secret), user.id);
    } else if (action == "confirm") {
        const auto pending = dbField<std::string>(rows[0]["totp_pending"]);
        const auto alive = co_await tx.query("SELECT 1 FROM users WHERE id=? AND totp_pending_expires_at>UTC_TIMESTAMP(6)", user.id);
        if (pending.empty() || alive.empty()) {
            co_return std::nullopt;
        }
        const auto secret = openSecret(key_, "pending:" + user.id, pending);
        const auto step = verifyTotp(secret, code, dbField<std::int64_t>(rows[0]["unix_time"]), -1);
        if (!step) {
            co_await failedAttempt(tx, user.id);
            co_await tx.commit();
            co_return std::nullopt;
        }
        co_await tx.execute("UPDATE users SET totp_secret=?,totp_last_step=?,totp_pending='',totp_pending_expires_at=NULL WHERE id=?", sealSecret(key_, "totp:" + user.id, secret), *step, user.id);
    } else if (action == "disable") {
        co_await tx.execute("UPDATE users SET totp_secret='',totp_last_step=-1,totp_pending='',totp_pending_expires_at=NULL WHERE id=?", user.id);
        co_await tx.repository<RecoveryCodeEntity>().delete_by(RecoveryCodeEntity::column<"user_id">() == user.id);
    } else if (action == "password") {
        co_await tx.execute("UPDATE users SET password_salt=?,password_hash=?,password_iterations=600000 WHERE id=?", newSalt, newDigest, user.id);
    }
    if (action == "confirm" || action == "recovery") {
        co_await tx.repository<RecoveryCodeEntity>().delete_by(RecoveryCodeEntity::column<"user_id">() == user.id);
        for (int i = 0; i < 10; ++i) {
            const auto recovery = randomToken().substr(0, 32);
            RecoveryCodeEntity entry;
            entry.set<"user_id">(user.id).set<"code_hash">(tokenHash(recovery));
            co_await tx.repository<RecoveryCodeEntity>().insert(entry);
            result.recoveryCodes.push_back(recovery);
        }
    }
    if (action != "begin") {
        co_await invalidateLogins(tx, user.id, user.loginHash);
    }
    co_await tx.execute("UPDATE users SET failed_logins=0,login_blocked_until=NULL WHERE id=?", user.id);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.security',true,?)", user.id, action);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<std::vector<AccountInfo>>> IdentityService::accounts(ruvia::db_executor db, UserIdentity actor) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    if (!(co_await adminAllowed(tx, actor))) {
        co_return std::nullopt;
    }
    std::vector<AccountInfo> result;
    const auto rows = co_await tx.query("SELECT id,username,role,disabled,totp_secret<>'' FROM users ORDER BY created_at LIMIT 1000");
    for (const auto& row : rows) {
        result.push_back({dbField<std::string>(row[0]), dbField<std::string>(row[1]), dbField<std::string>(row[2]), dbField<bool>(row[3]), dbField<bool>(row[4])});
    }
    co_await tx.commit();
    co_return result;
}
ruvia::task<bool> IdentityService::createAccount(ruvia::db_executor db, UserIdentity actor, std::string username, std::string password, std::string role, ruvia::context* context) const {
    if (!validUsername(username) || password.size() < 12 || password.size() > 1024 || (role != "admin" && role != "user")) {
        co_return false;
    }
    const auto salt = randomToken();
    const auto digest = co_await derivePasswordHash(context, password, salt, 600000);
    const auto id = "u-" + randomToken();
    auto tx = co_await db.begin_transaction(kReadCommitted);
    if (!(co_await adminAllowed(tx, actor))) {
        co_return false;
    }
    if (co_await tx.repository<UserEntity>().count({}) >= 1000) {
        co_return false;
    }
    if (co_await tx.repository<UserEntity>().exists({.where_ = UserEntity::column<"username">() == username})) {
        co_return false;
    }
    co_await tx.execute("INSERT INTO users(id,username,password_salt,password_hash,password_iterations,role) VALUES(?,?,?,?,600000,?)", id, username, salt, digest, role);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.create',true,?)", actor.id, id);
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> IdentityService::updateAccount(ruvia::db_executor db, UserIdentity actor, std::string id, std::string role, bool disabled, std::string password, ruvia::context* context) const {
    if ((role != "admin" && role != "user") || (!password.empty() && (password.size() < 12 || password.size() > 1024))) {
        co_return false;
    }
    std::string salt;
    std::string digest;
    if (!password.empty()) {
        salt = randomToken();
        digest = co_await derivePasswordHash(context, password, salt, 600000);
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    if (!(co_await adminAllowed(tx, actor))) {
        co_return false;
    }
    const auto rows = co_await tx.query("SELECT role,disabled FROM users WHERE id=? FOR UPDATE", id);
    if (rows.empty()) {
        co_return false;
    }
    if (dbField<std::string>(rows[0][0]) == "admin" && !dbField<bool>(rows[0][1]) && (role != "admin" || disabled)) {
        if (co_await tx.repository<UserEntity>().count({.where_ = UserEntity::column<"role">() == "admin" && UserEntity::column<"disabled">() == false}) <= 1) {
            co_return false;
        }
    }
    co_await tx.execute("UPDATE users SET role=?,disabled=? WHERE id=?", role, disabled, id);
    if (!password.empty()) {
        co_await tx.execute("UPDATE users SET password_salt=?,password_hash=?,password_iterations=600000,failed_logins=0,login_blocked_until=NULL WHERE id=?", salt, digest, id);
    }
    co_await invalidateLogins(tx, id, {});
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'identity.update',true,?)", actor.id, id);
    co_await tx.commit();
    co_return true;
}
} // namespace ultraterimal::server
