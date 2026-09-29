#pragma once
#include <ruvia/web/Controller.h>
#include <ruvia/web/RateLimit.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class TelegramController final : public ruvia::Controller<TelegramController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/telegram", state, UserAuthentication);
    RUVIA_POST("/telegram/configure", configure, BrowserOrigin, UserAuthentication, ruvia::RateLimit<5, 60000>);
    RUVIA_POST("/telegram/bind", bind, BrowserOrigin, UserAuthentication, ruvia::RateLimit<5, 60000>);
    RUVIA_POST("/telegram/unbind", unbind, BrowserOrigin, UserAuthentication, ruvia::RateLimit<5, 60000>);
    RUVIA_POST("/telegram/verify", verify, BrowserOrigin, UserAuthentication, ruvia::RateLimit<5, 60000>);
    RUVIA_POST("/telegram/webhook", webhook);
    RUVIA_POST("/approvals", request, BrowserOrigin, UserAuthentication, ruvia::RateLimit<10, 60000>);
    RUVIA_GET("/approvals", approvals, UserAuthentication);
    RUVIA_POST("/approvals/cancel", cancel, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> state(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> configure(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> bind(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> unbind(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> verify(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> webhook(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> request(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> approvals(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> cancel(ruvia::Context& context);
};
} // namespace ultraterimal::server
