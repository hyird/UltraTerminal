#pragma once
#include <asio/awaitable.hpp>
#include <event_loop.h>
#include <string>

#include "features/connection/connection.config.h"

namespace ultraterimal::agent {
// Returns a non-secret connection configuration after persisting current-account DPAPI credentials.
asio::awaitable<ConnectionConfig> enrollDevice(ruvia::event_loop loop, ConnectionConfig config, std::string input);
} // namespace ultraterimal::agent
