#pragma once
#include <controller.h>
#include <task.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class McpController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN(McpController)
    RUVIA_POST("/mcp", post, ApiTokenAuthentication);
    RUVIA_GET("/mcp", get, ApiTokenAuthentication);
    RUVIA_ROUTES_END

  private:
    ruvia::task<ruvia::http_response> post(ruvia::context& context);
    ruvia::task<ruvia::http_response> get(ruvia::context& context);
};
} // namespace ultraterimal::server
