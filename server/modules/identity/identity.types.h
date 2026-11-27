#pragma once
#include <cstdint>
#include <model.h>
#include <string>
#include <vector>

#include "common/http.h"

namespace ultraterimal::server {
struct UserIdentity {
    std::string id;
    std::string username;
    std::string role = "user";
    std::string loginHash;
};
enum class LoginError { kInvalidCredentials,
                        kFactorRequired };
struct LoginSession {
    UserIdentity user;
    std::string token;
};
struct AccountInfo {
    std::string id, username, role;
    bool disabled = false;
    bool totp = false;
};
struct SecurityInfo {
    bool totp = false;
    int recoveryCodes = 0;
};
struct TelegramVerification {
    std::string id;
    std::int64_t telegramId = 0;
};
struct SecurityChange {
    std::string secret;
    std::vector<std::string> recoveryCodes;
};

RUVIA_MODEL(LoginRequest,
            RUVIA_REQUIRED_FIELD(username, ruvia::string, RUVIA_NOT_EMPTY("username required"), RUVIA_MAX_BYTES(64, "username too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(password, ruvia::string, RUVIA_NOT_EMPTY("password required"), RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(ChangeSecurityRequest,
            RUVIA_REQUIRED_FIELD(action, ruvia::string, RUVIA_NOT_EMPTY("action required"), RUVIA_MAX_BYTES(16, "action too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(password, ruvia::string, RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(new_password, ruvia::string, RUVIA_MAX_BYTES(1024, "new_password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(CreateAccountRequest,
            RUVIA_REQUIRED_FIELD(username, ruvia::string, RUVIA_NOT_EMPTY("username required"), RUVIA_MAX_BYTES(64, "username too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(password, ruvia::string, RUVIA_NOT_EMPTY("password required"), RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(role, ruvia::string, RUVIA_NOT_EMPTY("role required"), RUVIA_MAX_BYTES(16, "role too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(current_password, ruvia::string, RUVIA_MAX_BYTES(1024, "current_password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(UpdateAccountRequest,
            RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_NOT_EMPTY("id required"), RUVIA_MAX_BYTES(128, "id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(password, ruvia::string, RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(role, ruvia::string, RUVIA_NOT_EMPTY("role required"), RUVIA_MAX_BYTES(16, "role too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(disabled, bool),
            RUVIA_OPTIONAL_FIELD(current_password, ruvia::string, RUVIA_MAX_BYTES(1024, "current_password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(UserResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(username, ruvia::string),
            RUVIA_REQUIRED_FIELD(role, ruvia::string));
RUVIA_MODEL(SecurityResponse,
            RUVIA_REQUIRED_FIELD(totp, bool),
            RUVIA_REQUIRED_FIELD(recovery_codes, std::int64_t));
RUVIA_MODEL(ChangeSecurityResponse,
            RUVIA_REQUIRED_FIELD(secret, ruvia::string),
            RUVIA_REQUIRED_FIELD(recovery_codes, ruvia::array<ruvia::string>));
RUVIA_MODEL(AccountItem,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(username, ruvia::string),
            RUVIA_REQUIRED_FIELD(role, ruvia::string),
            RUVIA_REQUIRED_FIELD(disabled, bool),
            RUVIA_REQUIRED_FIELD(totp, bool));
} // namespace ultraterimal::server
