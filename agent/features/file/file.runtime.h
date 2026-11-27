#pragma once
#include <asio/awaitable.hpp>
#include <event_loop.h>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/file.protocol.h"

namespace ultraterimal::agent {
asio::awaitable<void> runFileTransfer(ruvia::event_loop loop, ConnectionConfig config, wire::FileTransferStart request);
} // namespace ultraterimal::agent
