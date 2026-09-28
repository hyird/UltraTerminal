#pragma once
#include <ruvia/web/Controller.h>
#include <ruvia/web/RateLimit.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class SessionController final : public ruvia::Controller<SessionController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sessions")
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("", create, BrowserOrigin, UserAuthentication, ruvia::RateLimit<30, 60000>);
    RUVIA_GET("/:id", get, UserAuthentication);
    RUVIA_POST("/:id/close", close, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> create(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> get(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> close(ruvia::Context& context);
};
} // namespace ultraterimal::server
