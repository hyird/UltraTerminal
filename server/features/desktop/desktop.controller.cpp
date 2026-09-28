#include "desktop.controller.h"

#include "desktop_exchange.runtime.h"

namespace ultraterimal::server {
ruvia::Task<void> DesktopController::control(ruvia::Context& context) { co_await runDesktopChannel(context, context.webSocket(), protocol::ChannelSource::kBrowser, wire::DESKTOP_INPUT); }
ruvia::Task<void> DesktopController::video(ruvia::Context& context) { co_await runDesktopChannel(context, context.webSocket(), protocol::ChannelSource::kBrowser, wire::DESKTOP_VIDEO); }
} // namespace ultraterimal::server
