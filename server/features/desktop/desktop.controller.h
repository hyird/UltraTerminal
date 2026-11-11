#pragma once
#include <chrono>

#include <ruvia/core/task.h>
#include <ruvia/web/controller.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class DesktopController final {
  public:
    RUVIA_CONTROLLER_GROUP("/api/desktop")
    RUVIA_ROUTES_BEGIN(DesktopController)
    RUVIA_GET_WS("/control", control, socketOptions(), BrowserOrigin, UserAuthentication);
    RUVIA_GET_WS("/video", video, socketOptions(), BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    static ruvia::websocket_route_config socketOptions() {
        // The video channel can be idle independently of the input probes.
        return {.lifecycle_ = {.heartbeat_ = {.ping_interval_ = std::chrono::seconds(15), .pong_timeout_ = std::chrono::seconds(15)}}};
    }
    ruvia::task<void> control(ruvia::context& context);
    ruvia::task<void> video(ruvia::context& context);
};
} // namespace ultraterimal::server
