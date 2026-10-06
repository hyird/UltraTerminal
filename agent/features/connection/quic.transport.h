#pragma once

#include <asio/awaitable.hpp>
#include <cstddef>
#include <memory>
#include <string>

#include <ruvia/core/execution_handle.h>

#include "connection.config.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class QuicTransport {
  public:
    QuicTransport(ruvia::execution_handle loop, const ConnectionConfig& config, std::size_t maximumMessageBytes = protocol::kMaximumMessageBytes);
    ~QuicTransport();
    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;
    // False permits TLS fallback after an unavailable UDP path. Authentication
    // and protocol failures throw and must never trigger a silent downgrade.
    asio::awaitable<bool> connect();
    asio::awaitable<void> send(std::string bytes);
    asio::awaitable<std::string> receive();
    // Orderly shutdown after application I/O ends; bounded to two seconds.
    asio::awaitable<void> close();
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
