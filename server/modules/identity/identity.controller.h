#pragma once
#include <ruvia/web/Controller.h>
#include <ruvia/web/RateLimit.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class IdentityController final : public ruvia::Controller<IdentityController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/health", health);
    RUVIA_POST("/login", login, BrowserOrigin, ruvia::RateLimit<10, 60000>);
    RUVIA_POST("/logout", logout, BrowserOrigin, UserAuthentication);
    RUVIA_GET("/me", me, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> login(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> logout(ruvia::Context& context);
    ruvia::Task<ruvia::HttpResponse> me(ruvia::Context& context);
};
} // namespace ultraterimal::server
