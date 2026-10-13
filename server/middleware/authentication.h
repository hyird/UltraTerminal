#pragma once
#include <ruvia/web/controller.h>

#include "config/server.runtime.h"
namespace ultraterimal::server {
class BrowserOrigin final {
  public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        if (context.req().header("Origin").value_or("") != (*context.worker_state<std::shared_ptr<ServerRuntime>>()).config.origin) {
            // Finish the bounded request body before Ruvia closes a rejected request.
            (void)co_await context.req().text();
            context.respond(context.error({.status_ = ruvia::http_status::forbidden, .code_ = "origin_rejected", .message_ = "Origin rejected"}));
            co_return;
        }
        co_await next();
    }
};
class UserAuthentication final {
  public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto user = (co_await context.run_blocking([service = (*context.worker_state<std::shared_ptr<ServerRuntime>>()).identities, token = std::string(context.req().cookie("__Host-ultraterimal").value_or(""))] { return service.authenticate(token); }));
        if (!user) {
            (void)co_await context.req().text();
            context.respond(context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "unauthorized", .message_ = "Authentication required"}));
            co_return;
        }
        const auto binding = context.bind_request_state(*user);
        co_await next();
    }
};
} // namespace ultraterimal::server
