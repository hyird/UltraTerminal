#include "modules/tokens/token.controller.h"

#include <string>
#include <utility>
#include <vector>

#include "common/http.h"
#include "config/server.runtime.h"

namespace ultraterimal::server {
namespace {
ruvia::array<ruvia::string> toRuviaArray(const std::vector<std::string>& items) {
    ruvia::array<ruvia::string> out;
    out.reserve(items.size());
    for (const auto& item : items) {
        out.push_back(ruvia::string(item));
    }
    return out;
}
} // namespace

ruvia::task<ruvia::http_response> TokenController::list(ruvia::context& context) {
    const auto& user = context.request_state<UserIdentity>();
    const auto items = co_await context.worker_state<ServerRuntime>().tokens.list(context.db(), user.id);
    ruvia::array<TokenItem> response;
    response.reserve(items.size());
    for (const auto& token : items) {
        TokenItem row;
        row.set<"id">(token.id)
            .set<"name">(token.name)
            .set<"prefix">(token.prefix)
            .set<"scopes">(toRuviaArray(token.scopes))
            .set<"device_ids">(toRuviaArray(token.deviceIds))
            .set<"created_at">(token.createdAt);
        if (token.expiresAt) {
            row.set<"expires_at">(*token.expiresAt);
        } else {
            row.set_null<"expires_at">();
        }
        if (token.lastUsedAt) {
            row.set<"last_used_at">(*token.lastUsedAt);
        } else {
            row.set_null<"last_used_at">();
        }
        response.push_back(std::move(row));
    }
    co_return jsonResponse(context, response);
}

ruvia::task<ruvia::http_response> TokenController::create(ruvia::context& context, const CreateTokenRequest& body) {
    const auto& user = context.request_state<UserIdentity>();
    std::vector<std::string> scopes;
    for (const auto& s : body.get<"scopes">()) {
        scopes.emplace_back(s.view());
    }
    std::vector<std::string> deviceIds;
    if (const auto& ids = body.get<"device_ids">()) {
        for (const auto& id : *ids) {
            deviceIds.emplace_back(id.view());
        }
    }
    const auto created = co_await context.worker_state<ServerRuntime>().tokens.create(
        context.db(), user.id, textValue(body.get<"name">()), std::move(scopes), std::move(deviceIds), body.get<"expires_in_days">());
    if (!created) {
        if (created.error() == TokenCreateError::kLimitReached) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "token_limit", .message_ = "API token limit reached"});
        }
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "token_rejected", .message_ = "Invalid token configuration"});
    }

    CreateTokenResponse response;
    response.set<"id">(created->info.id)
        .set<"name">(created->info.name)
        .set<"prefix">(created->info.prefix)
        .set<"token">(created->rawToken)
        .set<"scopes">(toRuviaArray(created->info.scopes))
        .set<"device_ids">(toRuviaArray(created->info.deviceIds))
        .set<"created_at">(created->info.createdAt);
    if (created->info.expiresAt) {
        response.set<"expires_at">(*created->info.expiresAt);
    } else {
        response.set_null<"expires_at">();
    }
    co_return jsonResponse(context, response, {.status_ = ruvia::http_status::created});
}

ruvia::task<ruvia::http_response> TokenController::revoke(ruvia::context& context, const RevokeTokenRequest& body) {
    const auto& user = context.request_state<UserIdentity>();
    const bool ok = co_await context.worker_state<ServerRuntime>().tokens.revoke(context.db(), user.id, textValue(body.get<"id">()));
    if (!ok) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Token not found"});
    }
    co_return jsonResponse(context, okResponse());
}
} // namespace ultraterimal::server
