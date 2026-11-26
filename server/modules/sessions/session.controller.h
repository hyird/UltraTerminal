#pragma once
#include <controller.h>
#include <middleware.h>
#include <task.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class SessionController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sessions")
    RUVIA_ROUTES_BEGIN(SessionController)
    RUVIA_POST("", create, BrowserOrigin, UserAuthentication, ruvia::rate_limit<30, 60000>);
    RUVIA_GET("/:id", get, UserAuthentication);
    RUVIA_POST("/:id/close", close, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> create(ruvia::context& context);
    ruvia::task<ruvia::http_response> get(ruvia::context& context);
    ruvia::task<ruvia::http_response> close(ruvia::context& context);
};
} // namespace ultraterimal::server
