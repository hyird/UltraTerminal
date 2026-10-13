#pragma once

#include <asio/awaitable.hpp>
#include <cstdint>
#include <memory>

#include <ruvia/core/event_loop.h>

#include "send_queue.runtime.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
class DesktopTransport;
class DesktopWorkerChannel;
// Owns the native readers and the single writer for each native stream across
// worker replacement. Destroy only after run() and all worker bridges finish.
class DesktopRelay {
  public:
    DesktopRelay(ruvia::event_loop loop, DesktopTransport& transport);
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
    asio::awaitable<void> sendVideo(wire::Envelope message, std::uint32_t allowedOptimizationContract);
    asio::awaitable<void> sendCursor(wire::Envelope message);
    asio::awaitable<DesktopSendQueue::Entry> receiveInput();
    void completeInput(DesktopSendQueue::Entry& entry);
    std::uint32_t serverOptimizationContract() const;
    bool serverContinuousFrames() const;
    void cancel();
    bool cancelled() const;

  private:
    friend class DesktopRelay;
    struct State;
    explicit DesktopWorkerChannel(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
