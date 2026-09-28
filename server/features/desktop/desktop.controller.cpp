#include "desktop.controller.h"

#include "desktop_exchange.runtime.h"

namespace ultraterimal::server {
ruvia::Task<void> DesktopController::browser(ruvia::Context& context) { co_await runDesktopChannel(context, context.webSocket(), protocol::ChannelSource::kBrowser); }
} // namespace ultraterimal::server
