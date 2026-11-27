#pragma once
#include <controller.h>
#include <middleware.h>
#include <model_binding.h>
#include <security_middleware.h>
#include <task.h>

#include "common/http.h"
#include "identity.types.h"
#include "middleware/authentication.h"

namespace ultraterimal::server {
class IdentityController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api", ruvia::csrf_middleware)
    RUVIA_ROUTES_BEGIN(IdentityController)
    RUVIA_GET("/health", health);
    RUVIA_POST("/login", login, BrowserOrigin, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<LoginRequest>);
    RUVIA_POST("/logout", logout, BrowserOrigin, UserAuthentication, ruvia::strict_json_body<EmptyRequest>);
    RUVIA_GET("/me", me, UserAuthentication);
    RUVIA_GET("/security", security, UserAuthentication);
    RUVIA_POST("/security", changeSecurity, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<ChangeSecurityRequest>);
    RUVIA_GET("/users", accounts, UserAuthentication);
    RUVIA_POST("/users/create", createAccount, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<CreateAccountRequest>);
    RUVIA_POST("/users/update", updateAccount, BrowserOrigin, UserAuthentication, ruvia::rate_limit<10, 60000>, ruvia::strict_json_body<UpdateAccountRequest>);
    RUVIA_ROUTES_END
  private:
    ruvia::task<ruvia::http_response> security(ruvia::context& context);
    ruvia::task<ruvia::http_response> changeSecurity(ruvia::context& context, const ChangeSecurityRequest& body);
    ruvia::task<ruvia::http_response> accounts(ruvia::context& context);
    ruvia::task<ruvia::http_response> createAccount(ruvia::context& context, const CreateAccountRequest& body);
    ruvia::task<ruvia::http_response> updateAccount(ruvia::context& context, const UpdateAccountRequest& body);
    ruvia::task<ruvia::http_response> health(ruvia::context& context);
    ruvia::task<ruvia::http_response> login(ruvia::context& context, const LoginRequest& body);
    ruvia::task<ruvia::http_response> logout(ruvia::context& context);
    ruvia::task<ruvia::http_response> me(ruvia::context& context);
};
} // namespace ultraterimal::server
