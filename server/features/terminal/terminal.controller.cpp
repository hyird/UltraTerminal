#include "terminal.controller.h"

#include <streaming.h>

#include "terminal_exchange.runtime.h"

namespace ultraterimal::server {
ruvia::task<void> TerminalController::terminal(ruvia::context& context) {
    co_await runTerminalChannel(context, context.websocket(), protocol::ChannelSource::kBrowser);
}
} // namespace ultraterimal::server
