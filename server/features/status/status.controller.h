#pragma once
#include <ruvia/web/Controller.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class StatusController final : public ruvia::Controller<StatusController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_WS("/events", events, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::Task<void> events(ruvia::Context& context);
};
} // namespace ultraterimal::server
