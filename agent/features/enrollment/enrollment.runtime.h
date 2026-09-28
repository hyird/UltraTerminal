#pragma once
#include <asio/awaitable.hpp>
#include <string>

#include <ruvia/core/EventLoop.h>

#include "features/connection/connection.config.h"

namespace ultraterimal::agent {
// Returns a non-secret connection configuration after persisting current-account DPAPI credentials.
asio::awaitable<ConnectionConfig> enrollDevice(ruvia::EventLoop loop, ConnectionConfig config, std::string input);
} // namespace ultraterimal::agent
