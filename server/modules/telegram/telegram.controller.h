#pragma once
#include <controller.h>
#include <middleware.h>
#include <model_binding.h>
#include <security_middleware.h>
#include <task.h>

#include "common/http.h"
#include "middleware/authentication.h"
#include "telegram.types.h"

namespace ultraterimal::server {
class TelegramController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN(TelegramController)
    RUVIA_GET("/telegram", state, UserAuthentication);
    RUVIA_POST("/telegram/configure", configure, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>, ruvia::strict_json_body<TelegramConfigureRequest>);
    RUVIA_POST("/telegram/bind", bind, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>, ruvia::strict_json_body<TelegramReauthRequest>);
    RUVIA_POST("/telegram/unbind", unbind, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>, ruvia::strict_json_body<TelegramReauthRequest>);
    RUVIA_POST("/telegram/verify", verify, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<5, 60000>, ruvia::strict_json_body<EmptyRequest>);
    RUVIA_POST("/telegram/webhook", webhook);
    RUVIA_POST("/approvals", request, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<TelegramApprovalRequest>);
    RUVIA_GET("/approvals", approvals, UserAuthentication);
    RUVIA_POST("/approvals/cancel", cancel, ruvia::csrf_middleware, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<TelegramCancelRequest>);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> state(ruvia::context& context);
    ruvia::task<ruvia::http_response> configure(ruvia::context& context, const TelegramConfigureRequest& body);
    ruvia::task<ruvia::http_response> bind(ruvia::context& context, const TelegramReauthRequest& body);
    ruvia::task<ruvia::http_response> unbind(ruvia::context& context, const TelegramReauthRequest& body);
    ruvia::task<ruvia::http_response> verify(ruvia::context& context);
    ruvia::task<ruvia::http_response> webhook(ruvia::context& context);
    ruvia::task<ruvia::http_response> request(ruvia::context& context, const TelegramApprovalRequest& body);
    ruvia::task<ruvia::http_response> approvals(ruvia::context& context);
    ruvia::task<ruvia::http_response> cancel(ruvia::context& context, const TelegramCancelRequest& body);
};
} // namespace ultraterimal::server
