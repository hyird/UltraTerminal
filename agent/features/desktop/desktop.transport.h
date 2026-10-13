#pragma once
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <ruvia/core/event_loop.h>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Service-side transport: credentials stay here, never in the desktop process.
// All methods belong to one serial executor. One writer per channel, one input reader,
// and one video disconnect observer; callers join outstanding operations after cancel().
class DesktopTransport {
  public:
    DesktopTransport(ruvia::event_loop loop, ConnectionConfig config, wire::Envelope sessionOpen);
    ~DesktopTransport();
    DesktopTransport(const DesktopTransport&) = delete;
    DesktopTransport& operator=(const DesktopTransport&) = delete;
    asio::awaitable<void> connect(std::string credential);
    // Outgoing headers and sequence numbers are assigned here; callers supply the body and frame baseline.
    // Awaiting each send provides backpressure with at most one in-flight message per channel.
    // Returns the duration of actual native send, excluding queue/serialization.
    asio::awaitable<std::chrono::microseconds> sendVideo(wire::Envelope message, std::uint32_t allowedOptimizationContract);
    asio::awaitable<void> sendCursor(wire::Envelope message);
    asio::awaitable<wire::Envelope> receiveInput();
    asio::awaitable<void> watchVideo();
    // Actual readiness from BOTH native channels, not an assumed local schema.
    std::uint32_t serverOptimizationContract() const;
    bool serverContinuousFrames() const;
    void cancel();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
