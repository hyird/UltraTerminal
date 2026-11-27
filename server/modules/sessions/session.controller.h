#pragma once
#include <controller.h>
#include <middleware.h>
#include <model_binding.h>
#include <security_middleware.h>
#include <task.h>

#include "common/http.h"
#include "middleware/authentication.h"
#include "session.schema.h"
#include "session.types.h"

namespace ultraterimal::server {
class SessionController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sessions", ruvia::csrf_middleware)
    RUVIA_ROUTES_BEGIN(SessionController)
    RUVIA_POST("", create, BrowserOrigin, UserAuthentication, ruvia::rate_limit<30, 60000>, ruvia::strict_json_body<SessionCreateRequest>);
    RUVIA_GET("/:id", get, UserAuthentication);
    RUVIA_POST("/:id/close", close, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<EmptyRequest>);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> create(ruvia::context& context, const SessionCreateRequest& body);
    ruvia::task<ruvia::http_response> get(ruvia::context& context);
    ruvia::task<ruvia::http_response> close(ruvia::context& context);
};
} // namespace ultraterimal::server
