#pragma once
#include <ruvia/web/controller.h>
#include <ruvia/web/rate_limit.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class TelegramController final : public ruvia::controller<TelegramController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/telegram", state, UserAuthentication);
    RUVIA_POST("/telegram/configure", configure, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>);
    RUVIA_POST("/telegram/bind", bind, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>);
    RUVIA_POST("/telegram/unbind", unbind, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>);
    RUVIA_POST("/telegram/verify", verify, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>);
    RUVIA_POST("/telegram/webhook", webhook);
    RUVIA_POST("/approvals", request, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_GET("/approvals", approvals, UserAuthentication);
    RUVIA_POST("/approvals/cancel", cancel, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> state(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> configure(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> bind(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> unbind(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> verify(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> webhook(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> request(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> approvals(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> cancel(ruvia::context& context) const;
};
} // namespace ultraterimal::server
