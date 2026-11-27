#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <event_loop.h>
#include <memory>
#include <string>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
struct TerminalSessionResult {
    wire::Envelope terminal;
    std::uint32_t exitCode = 0;
};

class TerminalSession {
  public:
    TerminalSession(ruvia::event_loop loop, ConnectionConfig config, wire::Envelope sessionOpen);
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    asio::awaitable<TerminalSessionResult> run(std::string credential);
    void stop();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
