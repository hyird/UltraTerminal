#pragma once

#include <asio/awaitable.hpp>
#include <memory>
#include <string>

#include <ruvia/core/execution_handle.h>

#include "connection.config.h"
#include "ultraterimal/protocol/native.protocol.h"

namespace ultraterimal::agent {
// Authenticated native device connection; no HTTP or WebSocket dependency.
class DeviceTransport {
  public:
    DeviceTransport(ruvia::execution_handle loop, const ConnectionConfig& config, std::size_t maximumMessageBytes = protocol::kMaximumMessageBytes, wire::NativeConnect::Purpose purpose = wire::NativeConnect::CONTROL);
    ~DeviceTransport();
    DeviceTransport(const DeviceTransport&) = delete;
    DeviceTransport& operator=(const DeviceTransport&) = delete;
    asio::awaitable<void> connect(std::string credential);
    asio::awaitable<void> send(std::string bytes);
    asio::awaitable<std::string> receive();
    asio::awaitable<void> close();
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
