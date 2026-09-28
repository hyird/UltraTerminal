#pragma once
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <ruvia/core/EventLoop.h>

#include "connection.config.h"

namespace ultraterimal::agent {
struct ControlCheck {
    unsigned heartbeatSeconds = 0;
};
asio::awaitable<ControlCheck> checkControlConnection(ruvia::EventLoop loop, const ConnectionConfig& config);
struct ControlStatistics {
    std::uint64_t attempts = 0;
    std::uint64_t connections = 0;
    std::uint64_t acknowledgments = 0;
    std::uint64_t rejectedSessions = 0;
    std::uint64_t startedSessions = 0;
    std::uint64_t completedSessions = 0;
    bool connected = false;
};
// Run on the owning Ruvia event loop. Call run() once and await its
// completion before destroying the event loop.
class ControlRuntime {
  public:
    // Empty executable disables desktop dispatch for connection-only probes.
    ControlRuntime(ruvia::EventLoop loop, ConnectionConfig config, std::filesystem::path desktopExecutable = {});
    ~ControlRuntime();
    ControlRuntime(const ControlRuntime&) = delete;
    ControlRuntime& operator=(const ControlRuntime&) = delete;
    asio::awaitable<void> run();
    void stop();                          // Thread-safe stop request; the event loop cancels and joins its I/O.
    ControlStatistics statistics() const; // On the event loop or after run() completes.

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
