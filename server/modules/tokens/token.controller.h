#pragma once
#include <controller.h>
#include <middleware.h>
#include <model_binding.h>
#include <security_middleware.h>
#include <task.h>

#include "middleware/authentication.h"
#include "modules/tokens/token.types.h"

namespace ultraterimal::server {
class TokenController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api", ruvia::csrf_middleware)
    RUVIA_ROUTES_BEGIN(TokenController)
    RUVIA_GET("/tokens", list, UserAuthentication);
    RUVIA_POST("/tokens/create", create, BrowserOrigin, UserAuthentication, ruvia::rate_limit<20, 60000>, ruvia::strict_json_body<CreateTokenRequest>);
    RUVIA_POST("/tokens/revoke", revoke, BrowserOrigin, UserAuthentication, ruvia::rate_limit<30, 60000>, ruvia::strict_json_body<RevokeTokenRequest>);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> list(ruvia::context& context);
    ruvia::task<ruvia::http_response> create(ruvia::context& context, const CreateTokenRequest& body);
    ruvia::task<ruvia::http_response> revoke(ruvia::context& context, const RevokeTokenRequest& body);
};
} // namespace ultraterimal::server
