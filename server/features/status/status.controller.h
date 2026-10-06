#pragma once
#include <ruvia/web/controller.h>

#include "middleware/authentication.h"
namespace ultraterimal::server {
class StatusController final : public ruvia::controller<StatusController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_WS("/events", events, BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END
  private:
    ruvia::task<void> events(ruvia::context& context) const;
};
} // namespace ultraterimal::server
