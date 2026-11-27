#pragma once
#include <asio/awaitable.hpp>
#include <event_loop.h>
#include <filesystem>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::agent {
asio::awaitable<void> runDesktopSnapshot(ruvia::event_loop loop, ConnectionConfig config, std::filesystem::path desktopExecutable, wire::DesktopSnapshotStart request);
asio::awaitable<wire::DesktopActionResult> runDesktopAction(ruvia::event_loop loop, std::filesystem::path desktopExecutable, wire::DesktopActionRequest request);
} // namespace ultraterimal::agent
