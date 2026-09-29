#pragma once

#include <asio/awaitable.hpp>
#include <cstddef>
#include <memory>

#include <ruvia/core/EventLoop.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class DesktopTransport;
class DesktopWorkerChannel;
// Owns the native readers and the single writer for each native stream across
// worker replacement. Destroy only after run() and all worker bridges finish.
class DesktopRelay {
  public:
    DesktopRelay(ruvia::EventLoop loop, DesktopTransport& transport);
    ~DesktopRelay();
    asio::awaitable<void> run();
    std::shared_ptr<DesktopWorkerChannel> attach();
    void detach();
    void stop();
    wire::Envelope terminal() const;

  private:
    friend class DesktopWorkerChannel;
    struct State;
    std::unique_ptr<State> state_;
};
class DesktopWorkerChannel {
  public:
    ~DesktopWorkerChannel();
    asio::awaitable<void> sendVideo(wire::Envelope message);
    asio::awaitable<void> sendCursor(wire::Envelope message);
    asio::awaitable<wire::Envelope> receiveInput();
    void completeInput(std::size_t bytes);
    void cancel();
    bool cancelled() const;

  private:
    friend class DesktopRelay;
    struct State;
    explicit DesktopWorkerChannel(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
