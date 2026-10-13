#pragma once

#include <asio/awaitable.hpp>
#include <cstddef>
#include <memory>
#include <string>

#include <ruvia/core/event_loop.h>

#include "connection.config.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Raw TLS fallback. Authentication belongs to the native connection runtime.
// All operations, including cancel/destruction, belong to the owning Worker.
class TlsTransport {
  public:
    TlsTransport(ruvia::event_loop loop, const ConnectionConfig& config, std::size_t maximumMessageBytes = protocol::kMaximumMessageBytes);
    ~TlsTransport();
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;
    asio::awaitable<void> connect();
    asio::awaitable<void> send(std::string bytes);
    asio::awaitable<std::string> receive();
    asio::awaitable<void> close();
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
