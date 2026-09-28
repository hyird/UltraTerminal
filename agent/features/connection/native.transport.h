#pragma once

#include <asio/awaitable.hpp>
#include <memory>
#include <string>

#include <ruvia/core/EventLoop.h>

#include "quic.transport.h"
#include "tls.transport.h"

namespace ultraterimal::agent {
enum class NativeLink { kDisconnected,
                        kQuic,
                        kTls };
// Selection happens before application authentication. A failed live stream is
// closed; its owner must reauthenticate and rebuild the desktop baseline.
class NativeTransport {
  public:
    NativeTransport(ruvia::EventLoop loop, const ConnectionConfig& config, std::size_t maximumMessageBytes = protocol::kMaximumMessageBytes);
    ~NativeTransport();
    NativeTransport(const NativeTransport&) = delete;
    NativeTransport& operator=(const NativeTransport&) = delete;
    asio::awaitable<void> connect();
    asio::awaitable<void> send(std::string bytes);
    asio::awaitable<std::string> receive();
    // The owner must finish application reads/writes before orderly shutdown.
    asio::awaitable<void> close();
    NativeLink link() const;
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
