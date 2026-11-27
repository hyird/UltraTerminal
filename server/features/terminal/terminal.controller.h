#pragma once
#include <chrono>
#include <controller.h>
#include <task.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class TerminalController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api/sessions")
    RUVIA_ROUTES_BEGIN(TerminalController)
    RUVIA_GET_WS("/terminal", terminal, socketOptions(), BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    static ruvia::websocket_route_config socketOptions() {
        return {.lifecycle_ = {.heartbeat_ = {.ping_interval_ = std::chrono::seconds(15), .pong_timeout_ = std::chrono::seconds(15)}}};
    }
    ruvia::task<void> terminal(ruvia::context& context);
};
} // namespace ultraterimal::server
