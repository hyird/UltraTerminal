#pragma once
#include <controller.h>
#include <task.h>

#include "config/server.runtime.h"
namespace ultraterimal::server {
class BrowserOrigin final {
  public:
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        if (context.request().header("Origin").value_or("") != context.worker_state<ServerRuntime>().config.origin) {
            // Finish the bounded request body before Ruvia closes a rejected request.
            co_await context.request().discard_body();
            context.respond(context.error({.status_ = ruvia::http_status::forbidden, .code_ = "origin_rejected", .message_ = "Origin rejected"}));
            co_return;
        }
        co_await next();
    }
};
class UserAuthentication final {
  public:
    ruvia::task<void> invoke(ruvia::context& context, ruvia::next& next) {
        const auto user = co_await context.worker_state<ServerRuntime>().identities.authenticate(context.db(), std::string(context.request().cookie("__Host-ultraterimal").value_or("")));
        if (!user) {
            co_await context.request().discard_body();
            context.respond(context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "unauthorized", .message_ = "Authentication required"}));
            co_return;
        }
        const auto binding = context.bind_request_state(*user);
        co_await next();
    }
};
} // namespace ultraterimal::server
