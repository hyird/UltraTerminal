#include "modules/tokens/token.service.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "common/database.h"
#include "modules/identity/identity.service.h"

namespace ultraterimal::server {
namespace {
constexpr std::array<std::string_view, 6> kAllowedScopes{
    kTokenScopeDevicesRead,
    kTokenScopeFilesRead,
    kTokenScopeFilesWrite,
    kTokenScopeTerminalExec,
    kTokenScopeDesktopRead,
    kTokenScopeDesktopInput,
};

bool validIdentifier(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string joinCsv(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.append(items[i]);
    }
    return out;
}

std::vector<std::string> splitCsv(std::string_view csv) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? csv.size() : comma;
        if (end > start) {
            out.emplace_back(csv.substr(start, end - start));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

std::string trimCopy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}
} // namespace

bool validApiTokenFormat(std::string_view bearer) noexcept {
    if (!bearer.starts_with("utm_")) {
        return false;
    }
    return validToken(bearer.substr(4));
}

bool validTokenScope(std::string_view scope) noexcept {
    return std::find(kAllowedScopes.begin(), kAllowedScopes.end(), scope) != kAllowedScopes.end();
}

ruvia::task<std::vector<ApiTokenInfo>> TokenService::list(ruvia::db_executor db, std::string userId) const {
    const auto rows = co_await db.query(
        "SELECT id,name,token_prefix,scopes,device_ids,"
        "DATE_FORMAT(expires_at,'%Y-%m-%dT%H:%i:%s.%fZ'),"
        "DATE_FORMAT(last_used_at,'%Y-%m-%dT%H:%i:%s.%fZ'),"
        "DATE_FORMAT(created_at,'%Y-%m-%dT%H:%i:%s.%fZ') "
        "FROM api_tokens WHERE user_id=? AND revoked_at IS NULL "
        "ORDER BY created_at DESC LIMIT 100",
        userId);
    std::vector<ApiTokenInfo> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        out.push_back(ApiTokenInfo{
            dbField<std::string>(row[0]),
            dbField<std::string>(row[1]),
            dbField<std::string>(row[2]),
            splitCsv(dbField<std::string>(row[3])),
            splitCsv(dbField<std::string>(row[4])),
            dbOptional<std::string>(row[5]),
            dbOptional<std::string>(row[6]),
            dbField<std::string>(row[7]),
        });
    }
    co_return out;
}

ruvia::task<std::expected<CreatedApiToken, TokenCreateError>> TokenService::create(
    ruvia::db_executor db, std::string userId, std::string name,
    std::vector<std::string> scopes, std::vector<std::string> deviceIds,
    std::optional<std::int64_t> expiresInDays) const {
    name = trimCopy(name);
    if (name.empty() || name.size() > 128 || name.find('\0') != std::string::npos) {
        co_return std::unexpected(TokenCreateError::kInvalidInput);
    }
    if (expiresInDays && (*expiresInDays < 1 || *expiresInDays > 3650)) {
        co_return std::unexpected(TokenCreateError::kInvalidInput);
    }

    std::vector<std::string> normalizedScopes;
    for (const auto& rawScope : scopes) {
        if (!validTokenScope(rawScope)) {
            co_return std::unexpected(TokenCreateError::kInvalidInput);
        }
        if (std::find(normalizedScopes.begin(), normalizedScopes.end(), rawScope) == normalizedScopes.end()) {
            normalizedScopes.push_back(rawScope);
        }
    }
    if (normalizedScopes.empty()) {
        co_return std::unexpected(TokenCreateError::kInvalidInput);
    }

    std::vector<std::string> normalizedDevices;
    for (const auto& rawDevice : deviceIds) {
        if (!validIdentifier(rawDevice)) {
            co_return std::unexpected(TokenCreateError::kInvalidInput);
        }
        if (std::find(normalizedDevices.begin(), normalizedDevices.end(), rawDevice) == normalizedDevices.end()) {
            normalizedDevices.push_back(rawDevice);
        }
    }
    if (normalizedDevices.size() > 64) {
        co_return std::unexpected(TokenCreateError::kInvalidInput);
    }

    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto userRows = co_await tx.query("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", userId);
    if (userRows.empty()) {
        co_return std::unexpected(TokenCreateError::kUserUnavailable);
    }
    const auto countRows = co_await tx.query("SELECT count(*) FROM api_tokens WHERE user_id=? AND revoked_at IS NULL", userId);
    if (dbField<int>(countRows[0][0]) >= 50) {
        co_return std::unexpected(TokenCreateError::kLimitReached);
    }

    const std::string tokenId = "tok-" + randomToken();
    const std::string rawSecret = "utm_" + randomToken();
    const std::string prefix = rawSecret.substr(0, 12);
    const std::string hash = tokenHash(rawSecret);
    const std::string scopesCsv = joinCsv(normalizedScopes);
    const std::string devicesCsv = joinCsv(normalizedDevices);

    const auto inserted = expiresInDays
                              ? co_await tx.query(
                                    "INSERT INTO api_tokens(id,user_id,name,token_prefix,token_hash,scopes,device_ids,expires_at) "
                                    "VALUES(?,?,?,?,?,?,?,UTC_TIMESTAMP(6)+INTERVAL ? DAY) "
                                    "RETURNING DATE_FORMAT(expires_at,'%Y-%m-%dT%H:%i:%s.%fZ'),DATE_FORMAT(created_at,'%Y-%m-%dT%H:%i:%s.%fZ')",
                                    tokenId, userId, name, prefix, hash, scopesCsv, devicesCsv, *expiresInDays)
                              : co_await tx.query(
                                    "INSERT INTO api_tokens(id,user_id,name,token_prefix,token_hash,scopes,device_ids,expires_at) "
                                    "VALUES(?,?,?,?,?,?,?,NULL) "
                                    "RETURNING DATE_FORMAT(expires_at,'%Y-%m-%dT%H:%i:%s.%fZ'),DATE_FORMAT(created_at,'%Y-%m-%dT%H:%i:%s.%fZ')",
                                    tokenId, userId, name, prefix, hash, scopesCsv, devicesCsv);

    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'token.create',true,?)", userId, tokenId.substr(0, 128));
    co_await tx.commit();

    CreatedApiToken created;
    created.rawToken = rawSecret;
    created.info = ApiTokenInfo{
        tokenId,
        name,
        prefix,
        std::move(normalizedScopes),
        std::move(normalizedDevices),
        dbOptional<std::string>(inserted[0][0]),
        std::nullopt,
        dbField<std::string>(inserted[0][1]),
    };
    co_return created;
}

ruvia::task<bool> TokenService::revoke(ruvia::db_executor db, std::string userId, std::string tokenId) const {
    if (!validIdentifier(tokenId)) {
        co_return false;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto updated = co_await tx.execute(
        "UPDATE api_tokens SET revoked_at=UTC_TIMESTAMP(6) "
        "WHERE id=? AND user_id=? AND revoked_at IS NULL",
        tokenId, userId);
    if (updated.affected_rows() == 0) {
        co_await tx.rollback();
        co_return false;
    }
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success,reason) VALUES(?,'token.revoke',true,?)", userId, tokenId.substr(0, 128));
    co_await tx.commit();
    co_return true;
}

ruvia::task<std::optional<AuthenticatedToken>> TokenService::authenticate(ruvia::db_executor db, std::string bearerToken) const {
    if (!validApiTokenFormat(bearerToken)) {
        co_return std::nullopt;
    }
    const std::string hash = tokenHash(bearerToken);
    const auto rows = co_await db.query(
        "SELECT t.id,t.name,t.scopes,t.device_ids,u.id,u.username,u.role "
        "FROM api_tokens t JOIN users u ON u.id=t.user_id "
        "WHERE t.token_hash=? AND t.revoked_at IS NULL "
        "AND (t.expires_at IS NULL OR t.expires_at>UTC_TIMESTAMP(6)) AND NOT u.disabled",
        hash);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    AuthenticatedToken auth;
    auth.id = dbField<std::string>(rows[0][0]);
    auth.name = dbField<std::string>(rows[0][1]);
    auth.scopes = splitCsv(dbField<std::string>(rows[0][2]));
    auth.deviceIds = splitCsv(dbField<std::string>(rows[0][3]));
    auth.user = UserIdentity{
        dbField<std::string>(rows[0][4]),
        dbField<std::string>(rows[0][5]),
        dbField<std::string>(rows[0][6]),
        {},
    };
    try {
        (void)co_await db.execute("UPDATE api_tokens SET last_used_at=UTC_TIMESTAMP(6) WHERE id=?", auth.id);
    } catch (...) {
    }
    co_return auth;
}

ruvia::task<bool> TokenService::authorizeDevice(ruvia::db_executor db, const AuthenticatedToken& token, std::string_view deviceId,
                                                std::string_view requiredScope, bool requireWrite) const {
    if (!validIdentifier(deviceId) || !token.hasScope(requiredScope) || !token.allowsDevice(deviceId)) {
        co_return false;
    }
    const auto rows = co_await db.query(
        "SELECT 1 FROM devices d "
        "JOIN users u ON u.id=? "
        "JOIN device_grants g ON g.device_id=d.id AND g.user_id=u.id "
        "WHERE d.id=? AND d.revoked_at IS NULL AND NOT u.disabled "
        "AND (g.desktop OR g.ssh) AND (NOT ? OR g.write_control = TRUE)",
        token.user.id, std::string(deviceId), requireWrite);
    co_return !rows.empty();
}

ruvia::task<void> TokenService::auditToolCall(ruvia::db_executor db, const AuthenticatedToken& token, std::string deviceId,
                                              std::string toolName, bool success, std::string reason) const {
    try {
        const std::string event = ("mcp." + toolName).substr(0, 128);
        const std::string boundedReason = reason.substr(0, 128);
        if (!deviceId.empty() && validIdentifier(deviceId)) {
            const auto devRows = co_await db.query("SELECT id FROM devices WHERE id=?", deviceId);
            if (!devRows.empty()) {
                co_await db.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,?,?,?)",
                                    token.user.id, deviceId, event, success,
                                    boundedReason.empty() ? ruvia::db_value(nullptr) : ruvia::db_value(std::string_view(boundedReason)));
                co_return;
            }
        }
        co_await db.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,NULL,?,?,?)",
                            token.user.id, event, success,
                            boundedReason.empty() ? ruvia::db_value(nullptr) : ruvia::db_value(std::string_view(boundedReason)));
    } catch (...) {
    }
}
} // namespace ultraterimal::server
