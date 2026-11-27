#pragma once
#include <cstdint>
#include <db_executor.h>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <task.h>
#include <vector>

#include "modules/tokens/token.types.h"

namespace ultraterimal::server {
inline constexpr std::string_view kTokenScopeDevicesRead = "devices:read";
inline constexpr std::string_view kTokenScopeFilesRead = "files:read";
inline constexpr std::string_view kTokenScopeFilesWrite = "files:write";
inline constexpr std::string_view kTokenScopeTerminalExec = "terminal:exec";
inline constexpr std::string_view kTokenScopeDesktopRead = "desktop:read";
inline constexpr std::string_view kTokenScopeDesktopInput = "desktop:input";

bool validApiTokenFormat(std::string_view bearer) noexcept;
bool validTokenScope(std::string_view scope) noexcept;

class TokenService {
  public:
    ruvia::task<std::vector<ApiTokenInfo>> list(ruvia::db_executor db, std::string userId) const;
    ruvia::task<std::expected<CreatedApiToken, TokenCreateError>> create(ruvia::db_executor db, std::string userId, std::string name,
                                                                         std::vector<std::string> scopes, std::vector<std::string> deviceIds,
                                                                         std::optional<std::int64_t> expiresInDays) const;
    ruvia::task<bool> revoke(ruvia::db_executor db, std::string userId, std::string tokenId) const;
    ruvia::task<std::optional<AuthenticatedToken>> authenticate(ruvia::db_executor db, std::string bearerToken) const;
    ruvia::task<bool> authorizeDevice(ruvia::db_executor db, const AuthenticatedToken& token, std::string_view deviceId,
                                      std::string_view requiredScope, bool requireWrite) const;
    ruvia::task<void> auditToolCall(ruvia::db_executor db, const AuthenticatedToken& token, std::string deviceId,
                                    std::string toolName, bool success, std::string reason = {}) const;
};
} // namespace ultraterimal::server
