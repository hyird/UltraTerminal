#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <event_loop.h>

#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::agent {
class NamedPipeTransport;

wire::DesktopSnapshotResult executeDesktopSnapshot(const wire::DesktopSnapshotStart& request);
wire::DesktopActionResult executeDesktopAction(const wire::DesktopActionRequest& request);
asio::awaitable<std::uint32_t> runDesktopToolWorker(ruvia::event_loop loop, NamedPipeTransport& pipe);
} // namespace ultraterimal::agent
