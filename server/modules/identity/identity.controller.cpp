#include "identity.controller.h"

#include <array>
#include <chrono>

#include "config/request.schema.h"
#include "features/desktop/access.runtime.h"
#include "middleware/json.transport.h"
namespace ultraterimal::server {
ruvia::task<ruvia::http_response> IdentityController::health(ruvia::context& context) { co_return jsonResponse(context, {{"status", "ok"}}); }
ruvia::task<ruvia::http_response> IdentityController::login(ruvia::context& context) {
    const auto object = requestObject(co_await context.request().body_text(), {"username", "password", "code"});
    const auto session = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, username = requestText(object, "username", 64), password = requestText(object, "password", 1024), code = optionalText(object, "code", 32)] { return service.login(username, password, code); })).take_value());
    if (!session) {
        if (session.error() == LoginError::kFactorRequired) {
            co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "factor_required", .message_ = "Authenticator verification required"});
        }
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "invalid_login", .message_ = "Invalid credentials"});
    }
    const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "ultraterimal", .value_ = session->token, .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .max_age_ = std::chrono::seconds(28800), .prefix_ = ruvia::cookie_prefix::host, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
    co_return jsonResponse(context, {{"id", session->user.id}, {"username", session->user.username}, {"role", session->user.role}}, {.cookies_ = cookies});
}
ruvia::task<ruvia::http_response> IdentityController::logout(ruvia::context& context) {
    (void)requestObject(co_await context.request().body_text(), {});
    ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, token = std::string(context.request().cookie("__Host-ultraterimal").value_or(""))] { service.logout(token); })).take_value());
    co_await revalidateAccess(context, context.request_state<UserIdentity>().id);
    const std::array<ruvia::set_cookie_options, 1> cookies{{{.name_ = "ultraterimal", .value_ = "", .attributes_ = {.path_ = "/", .same_site_ = ruvia::cookie_same_site::strict, .max_age_ = std::chrono::seconds(0), .prefix_ = ruvia::cookie_prefix::host, .http_only_ = ruvia::cookie_attribute_policy::emit, .secure_ = ruvia::cookie_attribute_policy::emit}}}};
    co_return jsonResponse(context, {{"ok", true}}, {.cookies_ = cookies});
}
ruvia::task<ruvia::http_response> IdentityController::me(ruvia::context& context) {
    const auto& user = context.request_state<UserIdentity>();
    co_return jsonResponse(context, {{"id", user.id}, {"username", user.username}, {"role", user.role}});
}
ruvia::task<ruvia::http_response> IdentityController::security(ruvia::context& context) {
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, user = context.request_state<UserIdentity>().id] { return service.security(user); })).take_value());
    co_return jsonResponse(context, {{"totp", result.totp}, {"recovery_codes", result.recoveryCodes}});
}
ruvia::task<ruvia::http_response> IdentityController::changeSecurity(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"action", "password", "code", "new_password", "telegram_proof"});
    const auto user = context.request_state<UserIdentity>();
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, user, action = requestText(body, "action", 16), password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32), newPassword = optionalText(body, "new_password", 1024)] { return service.changeSecurity(user, action, password, code, newPassword, telegramProof); })).take_value());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "security_rejected", .message_ = "Verification failed"});
    }
    co_await revalidateAccess(context, user.id);
    boost::json::array recovery;
    for (const auto& code : result->recoveryCodes) {
        recovery.emplace_back(code);
    }
    co_return jsonResponse(context, {{"secret", result->secret}, {"recovery_codes", std::move(recovery)}});
}
ruvia::task<ruvia::http_response> IdentityController::accounts(ruvia::context& context) {
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, user = context.request_state<UserIdentity>()] { return service.accounts(user); })).take_value());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Admin required"});
    }
    boost::json::array accounts;
    for (const auto& account : *result) {
        accounts.push_back(boost::json::object{{"id", account.id}, {"username", account.username}, {"role", account.role}, {"disabled", account.disabled}, {"totp", account.totp}});
    }
    co_return jsonResponse(context, accounts);
}
ruvia::task<ruvia::http_response> IdentityController::createAccount(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"username", "password", "role", "current_password", "code", "telegram_proof"});
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, user = context.request_state<UserIdentity>(), username = requestText(body, "username", 64), password = requestText(body, "password", 1024), role = requestText(body, "role", 16), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.createAccount(user, username, password, role); })).take_value());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> IdentityController::updateAccount(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"id", "password", "role", "disabled", "current_password", "code", "telegram_proof"});
    const auto id = requestText(body, "id", 128);
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().identities, user = context.request_state<UserIdentity>(), id, password = optionalText(body, "password", 1024), role = requestText(body, "role", 16), disabled = requestBool(body, "disabled"), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.updateAccount(user, id, role, disabled, password); })).take_value());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_await revalidateAccess(context, id);
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
