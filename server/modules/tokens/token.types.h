#pragma once
#include <algorithm>
#include <cstdint>
#include <model.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/http.h"
#include "modules/identity/identity.types.h"

namespace ultraterimal::server {
struct ApiTokenInfo {
    std::string id;
    std::string name;
    std::string prefix;
    std::vector<std::string> scopes;
    std::vector<std::string> deviceIds;
    std::optional<std::string> expiresAt;
    std::optional<std::string> lastUsedAt;
    std::string createdAt;
};

struct CreatedApiToken {
    ApiTokenInfo info;
    std::string rawToken;
};

struct AuthenticatedToken {
    std::string id;
    std::string name;
    UserIdentity user;
    std::vector<std::string> scopes;
    std::vector<std::string> deviceIds;

    bool hasScope(std::string_view scope) const noexcept {
        return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
    }

    bool allowsDevice(std::string_view deviceId) const noexcept {
        if (deviceIds.empty()) {
            return true;
        }
        return std::find(deviceIds.begin(), deviceIds.end(), deviceId) != deviceIds.end();
    }
};

enum class TokenCreateError {
    kInvalidInput,
    kLimitReached,
    kUserUnavailable
};

RUVIA_MODEL(CreateTokenRequest,
            RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_NOT_EMPTY("name required"), RUVIA_MAX_BYTES(128, "name too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(scopes, ruvia::array<ruvia::string>),
            RUVIA_OPTIONAL_FIELD(device_ids, ruvia::array<ruvia::string>),
            RUVIA_OPTIONAL_FIELD(expires_in_days, std::int64_t, RUVIA_NULLABLE));

RUVIA_MODEL(RevokeTokenRequest,
            RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_NOT_EMPTY("id required"), RUVIA_MAX_BYTES(128, "id too long"), RUVIA_CUSTOM(noNul, "invalid character")));

RUVIA_MODEL(TokenItem,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(prefix, ruvia::string),
            RUVIA_REQUIRED_FIELD(scopes, ruvia::array<ruvia::string>),
            RUVIA_REQUIRED_FIELD(device_ids, ruvia::array<ruvia::string>),
            RUVIA_OPTIONAL_FIELD(expires_at, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
            RUVIA_OPTIONAL_FIELD(last_used_at, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
            RUVIA_REQUIRED_FIELD(created_at, ruvia::string));

RUVIA_MODEL(CreateTokenResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(prefix, ruvia::string),
            RUVIA_REQUIRED_FIELD(token, ruvia::string),
            RUVIA_REQUIRED_FIELD(scopes, ruvia::array<ruvia::string>),
            RUVIA_REQUIRED_FIELD(device_ids, ruvia::array<ruvia::string>),
            RUVIA_OPTIONAL_FIELD(expires_at, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
            RUVIA_REQUIRED_FIELD(created_at, ruvia::string));
} // namespace ultraterimal::server
