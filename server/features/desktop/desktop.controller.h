#pragma once
#include <chrono>

#include <ruvia/web/Controller.h>

#include "middleware/authentication.h"

namespace ultraterimal::server {
class DesktopController final : public ruvia::Controller<DesktopController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/desktop")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET_WS_OPTIONS("/control", control, socketOptions(), BrowserOrigin, UserAuthentication);
    RUVIA_GET_WS_OPTIONS("/video", video, socketOptions(), BrowserOrigin, UserAuthentication);
    RUVIA_ROUTES_END

  private:
    static ruvia::WebSocketRouteConfig socketOptions() {
        // The video channel can be idle independently of the input probes.
        return {.lifecycle = {.heartbeat = {.pingInterval = std::chrono::seconds(15), .pongTimeout = std::chrono::seconds(15)}}};
    }
    ruvia::Task<void> control(ruvia::Context& context);
    ruvia::Task<void> video(ruvia::Context& context);
};
} // namespace ultraterimal::server
