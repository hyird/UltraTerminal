#pragma once
#include <controller.h>
#include <task.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class StatusController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN(StatusController)
    RUVIA_GET_WS("/events", events, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::task<void> events(ruvia::context& context);
};
} // namespace ultraterimal::server
