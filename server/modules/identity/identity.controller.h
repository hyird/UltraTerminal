#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/rate_limit.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class IdentityController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN(IdentityController)
    RUVIA_GET("/health", health);
    RUVIA_POST("/login", login, BrowserOrigin, ruvia::rate_limit<10, 60000>);
    RUVIA_POST("/logout", logout, BrowserOrigin, UserAuthentication);
    RUVIA_GET("/me", me, UserAuthentication);
    RUVIA_GET("/security", security, UserAuthentication);
    RUVIA_POST("/security", changeSecurity, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_GET("/users", accounts, UserAuthentication);
    RUVIA_POST("/users/create", createAccount, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_POST("/users/update", updateAccount, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> security(ruvia::context& context);
    ruvia::task<ruvia::http_response> changeSecurity(ruvia::context& context);
    ruvia::task<ruvia::http_response> accounts(ruvia::context& context);
    ruvia::task<ruvia::http_response> createAccount(ruvia::context& context);
    ruvia::task<ruvia::http_response> updateAccount(ruvia::context& context);
    ruvia::task<ruvia::http_response> health(ruvia::context& context);
    ruvia::task<ruvia::http_response> login(ruvia::context& context);
    ruvia::task<ruvia::http_response> logout(ruvia::context& context);
    ruvia::task<ruvia::http_response> me(ruvia::context& context);
};
} // namespace ultraterimal::server
