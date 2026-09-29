#include "identity.controller.h"

#include "config/request.schema.h"
#include "features/desktop/access.runtime.h"
#include "middleware/json.transport.h"
namespace ultraterimal::server {
ruvia::Task<ruvia::HttpResponse> IdentityController::health(ruvia::Context& context) { co_return jsonResponse(context, {{"status", "ok"}}); }
ruvia::Task<ruvia::HttpResponse> IdentityController::login(ruvia::Context& context) {
    const auto object = requestObject(co_await context.req().text(), {"username", "password", "code"});
    const auto session = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, username = requestText(object, "username", 64), password = requestText(object, "password", 1024), code = optionalText(object, "code", 32)] { return service.login(username, password, code); });
    if (!session) {
        if (session.error() == LoginError::kFactorRequired) {
            co_return context.error({.status = ruvia::http_status::kUnauthorized, .code = "factor_required", .message = "Authenticator verification required"});
        }
        co_return context.error({.status = ruvia::http_status::kUnauthorized, .code = "invalid_login", .message = "Invalid credentials"});
    }
    context.header("Set-Cookie", "__Host-ultraterimal=" + session->token + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=28800");
    co_return jsonResponse(context, {{"id", session->user.id}, {"username", session->user.username}, {"role", session->user.role}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::logout(ruvia::Context& context) {
    (void)requestObject(co_await context.req().text(), {});
    co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""))] { service.logout(token); });
    co_await revalidateAccess(context, context.requestState<UserIdentity>().id);
    context.header("Set-Cookie", "__Host-ultraterimal=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::me(ruvia::Context& context) {
    const auto& user = context.requestState<UserIdentity>();
    co_return jsonResponse(context, {{"id", user.id}, {"username", user.username}, {"role", user.role}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::security(ruvia::Context& context) {
    const auto result = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, user = context.requestState<UserIdentity>().id] { return service.security(user); });
    co_return jsonResponse(context, {{"totp", result.totp}, {"recovery_codes", result.recoveryCodes}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::changeSecurity(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"action", "password", "code", "new_password", "telegram_proof"});
    const auto user = context.requestState<UserIdentity>();
    const auto result = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, user, action = requestText(body, "action", 16), password = optionalText(body, "password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32), newPassword = optionalText(body, "new_password", 1024)] { return service.changeSecurity(user, action, password, code, newPassword, telegramProof); });
    if (!result) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "security_rejected", .message = "Verification failed"});
    }
    co_await revalidateAccess(context, user.id);
    boost::json::array recovery;
    for (const auto& code : result->recoveryCodes) {
        recovery.emplace_back(code);
    }
    co_return jsonResponse(context, {{"secret", result->secret}, {"recovery_codes", std::move(recovery)}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::accounts(ruvia::Context& context) {
    const auto result = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, user = context.requestState<UserIdentity>()] { return service.accounts(user); });
    if (!result) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Admin required"});
    }
    boost::json::array accounts;
    for (const auto& account : *result) {
        accounts.push_back(boost::json::object{{"id", account.id}, {"username", account.username}, {"role", account.role}, {"disabled", account.disabled}, {"totp", account.totp}});
    }
    co_return jsonResponse(context, accounts);
}
ruvia::Task<ruvia::HttpResponse> IdentityController::createAccount(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"username", "password", "role", "current_password", "code", "telegram_proof"});
    const auto result = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, user = context.requestState<UserIdentity>(), username = requestText(body, "username", 64), password = requestText(body, "password", 1024), role = requestText(body, "role", 16), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.createAccount(user, username, password, role); });
    if (!result) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "account_rejected", .message = "Account operation rejected"});
    }
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::updateAccount(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"id", "password", "role", "disabled", "current_password", "code", "telegram_proof"});
    const auto id = requestText(body, "id", 128);
    const auto result = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, user = context.requestState<UserIdentity>(), id, password = optionalText(body, "password", 1024), role = requestText(body, "role", 16), disabled = requestBool(body, "disabled"), current = optionalText(body, "current_password", 1024), telegramProof = optionalText(body, "telegram_proof", 32), code = optionalText(body, "code", 32)] { return user.role == "admin" && service.reauthenticate(user, current, code, telegramProof) && service.updateAccount(user, id, role, disabled, password); });
    if (!result) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "account_rejected", .message = "Account operation rejected"});
    }
    co_await revalidateAccess(context, id);
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
