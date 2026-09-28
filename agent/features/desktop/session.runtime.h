#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <ruvia/core/EventLoop.h>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
struct DesktopSessionResult {
    wire::Envelope terminal;
    std::uint32_t processId = 0;
    std::uint32_t exitCode = 0;
    bool forcedExit = false;
};
// One authorized desktop session on one event loop. The control owner supplies
// the selected Windows session and trusted installed executable. run() pairs
// native channels, launches the child off-loop, bridges IPC and joins the child on every exit.
// stop() belongs to this event loop; await run() before destroying the owner.
class DesktopSession {
  public:
    DesktopSession(ruvia::EventLoop loop, ConnectionConfig config, wire::Envelope grant,
                   std::filesystem::path executable, std::uint32_t windowsSession);
    ~DesktopSession();
    DesktopSession(const DesktopSession&) = delete;
    DesktopSession& operator=(const DesktopSession&) = delete;
    asio::awaitable<DesktopSessionResult> run(std::string credential);
    void stop();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
