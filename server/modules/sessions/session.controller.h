#pragma once
#include <ruvia/web/controller.h>
#include <ruvia/web/rate_limit.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class SessionController final : public ruvia::controller<SessionController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sessions")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("", create, BrowserOrigin, UserAuthentication, ruvia::rate_limit<30, 60000>);
    RUVIA_GET("/:id", get, UserAuthentication);
    RUVIA_POST("/:id/close", close, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> create(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> get(ruvia::context& context) const;
    ruvia::task<ruvia::http_response> close(ruvia::context& context) const;
};
} // namespace ultraterimal::server
