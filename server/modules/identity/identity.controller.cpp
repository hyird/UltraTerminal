#include "identity.controller.h"

#include "config/request.schema.h"
#include "features/desktop/access.runtime.h"
#include "middleware/json.transport.h"
namespace ultraterimal::server {
ruvia::task<ruvia::http_response> IdentityController::health(ruvia::context& context) const { co_return jsonResponse(context, {{"status", "ok"}}); }
ruvia::task<ruvia::http_response> IdentityController::login(ruvia::context& context) const {
    const auto object = requestObject(co_await context.req().text(), {"username", "password", "code"});
    const auto session = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, username = requestText(object, "username", 64), password = requestText(object, "password", 1024), code = optionalText(object, "code", 32)] { return service.login(username, password, code); })).value();
    if (!session) {
        if (session.error() == LoginError::kFactorRequired) {
            co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "factor_required", .message_ = "Authenticator verification required"});
        }
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "invalid_login", .message_ = "Invalid credentials"});
    }
    context.header("Set-Cookie", "__Host-ultraterimal=" + session->token + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=28800");
    co_return jsonResponse(context, {{"id", session->user.id}, {"username", session->user.username}, {"role", session->user.role}});
}
ruvia::task<ruvia::http_response> IdentityController::logout(ruvia::context& context) const {
    (void)requestObject(co_await context.req().text(), {});
    (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""))] { service.logout(token); })).value();
    co_await revalidateAccess(context, context.request_state<UserIdentity>().id);
    context.header("Set-Cookie", "__Host-ultraterimal=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> IdentityController::me(ruvia::context& context) const {
    const auto& user = context.request_state<UserIdentity>();
    co_return jsonResponse(context, {{"id", user.id}, {"username", user.username}, {"role", user.role}});
}
ruvia::task<ruvia::http_response> IdentityController::security(ruvia::context& context) const {
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, user = context.request_state<UserIdentity>().id] { return service.security(user); })).value();
    co_return jsonResponse(context, {{"totp", result.totp}, {"recovery_codes", result.recoveryCodes}});
}
ruvia::task<ruvia::http_response> IdentityController::changeSecurity(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"action", "password", "code", "new_password", "telegram_proof"});
    const auto user = context.request_state<UserIdentity>();
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, user, action = requestText(body, "action", 16), password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32), newPassword = optionalText(body, "new_password", 1024)] { return service.changeSecurity(user, action, password, code, newPassword, telegramProof); })).value();
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
ruvia::task<ruvia::http_response> IdentityController::accounts(ruvia::context& context) const {
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, user = context.request_state<UserIdentity>()] { return service.accounts(user); })).value();
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Admin required"});
    }
    boost::json::array accounts;
    for (const auto& account : *result) {
        accounts.push_back(boost::json::object{{"id", account.id}, {"username", account.username}, {"role", account.role}, {"disabled", account.disabled}, {"totp", account.totp}});
    }
    co_return jsonResponse(context, accounts);
}
ruvia::task<ruvia::http_response> IdentityController::createAccount(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"username", "password", "role", "current_password", "code", "telegram_proof"});
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, user = context.request_state<UserIdentity>(), username = requestText(body, "username", 64), password = requestText(body, "password", 1024), role = requestText(body, "role", 16), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.createAccount(user, username, password, role); })).value();
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::task<ruvia::http_response> IdentityController::updateAccount(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"id", "password", "role", "disabled", "current_password", "code", "telegram_proof"});
    const auto id = requestText(body, "id", 128);
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).identities, user = context.request_state<UserIdentity>(), id, password = optionalText(body, "password", 1024), role = requestText(body, "role", 16), disabled = requestBool(body, "disabled"), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.updateAccount(user, id, role, disabled, password); })).value();
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "account_rejected", .message_ = "Account operation rejected"});
    }
    co_await revalidateAccess(context, id);
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
