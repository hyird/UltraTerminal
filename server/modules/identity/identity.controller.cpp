#include "identity.controller.h"

#include "config/request.schema.h"
#include "middleware/json.transport.h"
namespace ultraterimal::server {
ruvia::Task<ruvia::HttpResponse> IdentityController::health(ruvia::Context& context) { co_return jsonResponse(context, {{"status", "ok"}}); }
ruvia::Task<ruvia::HttpResponse> IdentityController::login(ruvia::Context& context) {
    const auto object = requestObject(co_await context.req().text(), {"username", "password"});
    const auto session = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, username = requestText(object, "username", 64), password = requestText(object, "password", 1024)] { return service.login(username, password); });
    if (!session) {
        co_return context.error({.status = ruvia::http_status::kUnauthorized, .code = "invalid_login", .message = "Invalid credentials"});
    }
    context.header("Set-Cookie", "__Host-ultraterimal=" + session->token + "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=28800");
    co_return jsonResponse(context, {{"id", session->user.id}, {"username", session->user.username}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::logout(ruvia::Context& context) {
    (void)requestObject(co_await context.req().text(), {});
    co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""))] { service.logout(token); });
    context.header("Set-Cookie", "__Host-ultraterimal=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
    co_return jsonResponse(context, {{"ok", true}});
}
ruvia::Task<ruvia::HttpResponse> IdentityController::me(ruvia::Context& context) {
    const auto& user = context.requestState<UserIdentity>();
    co_return jsonResponse(context, {{"id", user.id}, {"username", user.username}});
}
} // namespace ultraterimal::server
