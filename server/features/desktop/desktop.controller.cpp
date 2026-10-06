#include "desktop.controller.h"

#include "desktop_exchange.runtime.h"

namespace ultraterimal::server {
ruvia::task<void> DesktopController::control(ruvia::context& context) const { co_await runDesktopChannel(context, context.websocket(), protocol::ChannelSource::kBrowser, wire::DESKTOP_INPUT); }
ruvia::task<void> DesktopController::video(ruvia::context& context) const { co_await runDesktopChannel(context, context.websocket(), protocol::ChannelSource::kBrowser, wire::DESKTOP_VIDEO); }
} // namespace ultraterimal::server
