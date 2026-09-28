#pragma once
#include <ruvia/web/Controller.h>

#include "config/server.runtime.h"
namespace ultraterimal::server {
class BrowserOrigin final : public ruvia::Middleware {
  public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        if (context.req().header("Origin").value_or("") != context.workerState<ServerRuntime>().config.origin) {
            // Finish the bounded request body before Ruvia closes a rejected request.
            (void)co_await context.req().text();
            context.respond(context.error({.status = ruvia::http_status::kForbidden, .code = "origin_rejected", .message = "Origin rejected"}));
            co_return;
        }
        co_await next();
    }
};
class UserAuthentication final : public ruvia::Middleware {
  public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto user = co_await context.runBlocking([service = context.workerState<ServerRuntime>().identities, token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""))] { return service.authenticate(token); });
        if (!user) {
            (void)co_await context.req().text();
            context.respond(context.error({.status = ruvia::http_status::kUnauthorized, .code = "unauthorized", .message = "Authentication required"}));
            co_return;
        }
        const auto binding = context.bindRequestState(*user);
        co_await next();
    }
};
} // namespace ultraterimal::server
