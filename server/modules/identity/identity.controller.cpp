#include "identity.controller.h"

#include <array>
#include <chrono>

#include "features/desktop/access.runtime.h"

namespace ultraterimal::server {
ruvia::task<ruvia::http_response> IdentityController::health(ruvia::context& context) {
    context.policy().header("Cache-Control", "no-store");
    co_return ruvia::make_health_response(context);
}
ruvia::task<ruvia::http_response> IdentityController::login(ruvia::context& context, const LoginRequest& body) {
    const auto session = co_await context.worker_state<ServerRuntime>().identities.login(context.db(), textValue(body.get<"username">()), textValue(body.get<"password">()), optionalTextValue(body.get<"code">()), &context);
    if (!session) {
        if (session.error() == LoginError::kFactorRequired) {
            co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "factor_required", .message_ = "Authenticator verification required"});
        }
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "invalid_login", .message_ = "Invalid credentials"});
    }
    const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "ultraterimal", .value_ = session->token, .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .max_age_ = std::chrono::seconds(28800), .prefix_ = ruvia::cookie_prefix::host, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
    UserResponse response;
    response.set<"id">(session->user.id).set<"username">(session->user.username).set<"role">(session->user.role);
    co_return jsonResponse(context, response, {.cookies_ = cookies});
}
ruvia::task<ruvia::http_response> IdentityController::logout(ruvia::context& context) {
    co_await context.worker_state<ServerRuntime>().identities.logout(context.db(), std::string(context.request().cookie("__Host-ultraterimal").value_or("")));
    co_await revalidateAccess(context, context.request_state<UserIdentity>().id);
    const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "ultraterimal", .value_ = "", .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .max_age_ = std::chrono::seconds(0), .prefix_ = ruvia::cookie_prefix::host, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
    co_return jsonResponse(context, okResponse(), {.cookies_ = cookies});
}
ruvia::task<ruvia::http_response> IdentityController::me(ruvia::context& context) {
    const auto& user = context.request_state<UserIdentity>();
    UserResponse response;
    response.set<"id">(user.id).set<"username">(user.username).set<"role">(user.role);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> IdentityController::security(ruvia::context& context) {
    const auto result = co_await context.worker_state<ServerRuntime>().identities.security(context.db(), context.request_state<UserIdentity>().id);
    SecurityResponse response;
    response.set<"totp">(result.totp).set<"recovery_codes">(static_cast<std::int64_t>(result.recoveryCodes));
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> IdentityController::changeSecurity(ruvia::context& context, const ChangeSecurityRequest& body) {
    const auto user = context.request_state<UserIdentity>();
    const auto result = co_await context.worker_state<ServerRuntime>().identities.changeSecurity(context.db(), user, textValue(body.get<"action">()), optionalTextValue(body.get<"password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"new_password">()), optionalTextValue(body.get<"telegram_proof">()), &context);
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "security_rejected", .message_ = "Verification failed"});
    }
    co_await revalidateAccess(context, user.id);
    ChangeSecurityResponse response;
    response.set<"secret">(result->secret);
    auto& recovery = response.ensure<"recovery_codes">();
    for (const auto& code : result->recoveryCodes) {
        recovery.push_back(ruvia::string(code));
    }
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> IdentityController::accounts(ruvia::context& context) {
    const auto result = co_await context.worker_state<ServerRuntime>().identities.accounts(context.db(), context.request_state<UserIdentity>());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Admin required"});
    }
    ruvia::array<AccountItem> accounts;
    for (const auto& account : *result) {
        AccountItem item;
        item.set<"id">(account.id).set<"username">(account.username).set<"role">(account.role).set<"disabled">(account.disabled).set<"totp">(account.totp);
        accounts.push_back(std::move(item));
    }
    co_return jsonResponse(context, accounts);
}
ruvia::task<ruvia::http_response> IdentityController::createAccount(ruvia::context& context, const CreateAccountRequest& body) {
    const auto& service = context.worker_state<ServerRuntime>().identities;
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    const bool result = user.role == "admin" && (co_await service.reauthenticate(db, user, optionalTextValue(body.get<"current_password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()), &context)) && (co_await service.createAccount(db, user, textValue(body.get<"username">()), textValue(body.get<"password">()), textValue(body.get<"role">()), &context));
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_return jsonResponse(context, okResponse());
}
ruvia::task<ruvia::http_response> IdentityController::updateAccount(ruvia::context& context, const UpdateAccountRequest& body) {
    const auto id = textValue(body.get<"id">());
    const auto& service = context.worker_state<ServerRuntime>().identities;
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    const bool result = user.role == "admin" && (co_await service.reauthenticate(db, user, optionalTextValue(body.get<"current_password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()), &context)) && (co_await service.updateAccount(db, user, id, textValue(body.get<"role">()), body.get<"disabled">(), optionalTextValue(body.get<"password">()), &context));
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_await revalidateAccess(context, id);
    co_return jsonResponse(context, okResponse());
}
} // namespace ultraterimal::server
