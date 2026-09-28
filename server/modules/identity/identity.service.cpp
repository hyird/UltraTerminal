#include "identity.service.h"

#include <array>
#include <span>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "modules/database/database.service.h"
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
    pqxx::work tx(connection);
    tx.exec("SELECT pg_advisory_xact_lock(842105212)");
    if (!tx.exec("SELECT id FROM users LIMIT 1").empty()) {
        throw std::runtime_error("Bootstrap already completed");
    }
    tx.exec("INSERT INTO users(id,username,password_salt,password_hash,password_iterations) VALUES($1,$2,$3,$4,600000)", pqxx::params(tx, id, username, salt, digest));
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES($1,'identity.bootstrap',true)", pqxx::params(tx, id));
    tx.commit();
}
std::optional<LoginSession> IdentityService::login(std::string_view username, std::string_view password) const {
    if (username.size() > 64 || password.empty() || password.size() > 1024) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("SELECT id,username,password_salt,password_hash,password_iterations FROM users WHERE username=$1 AND NOT disabled FOR UPDATE", pqxx::params(tx, username));
    const auto digest = passwordHash(password, rows.empty() ? std::string(64, '0') : rows[0]["password_salt"].as<std::string>(), rows.empty() ? 600000 : rows[0]["password_iterations"].as<int>());
    const auto expected = rows.empty() ? std::string(64, '0') : rows[0]["password_hash"].as<std::string>();
    if (rows.empty() || expected.size() != digest.size() || CRYPTO_memcmp(expected.data(), digest.data(), digest.size()) != 0) {
        tx.exec("INSERT INTO audit_events(event_type,success) VALUES('identity.login',false)");
        tx.commit();
        return std::nullopt;
    }
    LoginSession result{{rows[0]["id"].as<std::string>(), rows[0]["username"].as<std::string>()}, randomToken()};
    tx.exec("DELETE FROM login_sessions WHERE expires_at<=now() OR token_hash IN (SELECT token_hash FROM login_sessions WHERE user_id=$1 ORDER BY created_at DESC OFFSET 9)", pqxx::params(tx, result.user.id));
    tx.exec("INSERT INTO login_sessions(token_hash,user_id,expires_at) VALUES($1,$2,now()+interval '8 hours')", pqxx::params(tx, tokenHash(result.token), result.user.id));
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES($1,'identity.login',true)", pqxx::params(tx, result.user.id));
    tx.commit();
    return result;
}
bool IdentityService::verifyPassword(std::string_view userId, std::string_view password) const {
    if (userId.empty() || password.empty() || password.size() > 1024) {
        return false;
    }
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT password_salt,password_hash,password_iterations FROM users WHERE id=$1 AND NOT disabled", pqxx::params(tx, userId));
    const auto digest = passwordHash(password, rows.empty() ? std::string(64, '0') : rows[0]["password_salt"].as<std::string>(), rows.empty() ? 600000 : rows[0]["password_iterations"].as<int>());
    const auto expected = rows.empty() ? std::string(64, '0') : rows[0]["password_hash"].as<std::string>();
    return !rows.empty() && expected.size() == digest.size() && CRYPTO_memcmp(expected.data(), digest.data(), digest.size()) == 0;
}
std::optional<UserIdentity> IdentityService::authenticate(std::string_view token) const {
    if (!validToken(token)) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT u.id,u.username FROM users u JOIN login_sessions s ON s.user_id=u.id WHERE s.token_hash=$1 AND s.expires_at>now() AND NOT u.disabled", pqxx::params(tx, tokenHash(token)));
    if (rows.empty()) {
        return std::nullopt;
    }
    return UserIdentity{rows[0][0].as<std::string>(), rows[0][1].as<std::string>()};
}
void IdentityService::logout(std::string_view token) const {
    if (!validToken(token)) {
        return;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    tx.exec("DELETE FROM login_sessions WHERE token_hash=$1", pqxx::params(tx, tokenHash(token)));
    tx.commit();
}
} // namespace ultraterimal::server
