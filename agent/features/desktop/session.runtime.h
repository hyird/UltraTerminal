#pragma once
#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <ruvia/core/EventLoop.h>

#include "features/connection/connection.config.h"
#include "ultraterimal/protocol/desktop_failure.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
struct DesktopSessionResult {
    wire::Envelope terminal;
    std::uint32_t processId = 0;
    std::uint32_t exitCode = 0;
    bool forcedExit = false;
    std::optional<wire::DesktopFault> fault;
    bool hasIndependentFailure() const {
        if (!fault) {
            return false;
        }
        using Fault = wire::DesktopFault;
        // These failures are not consequences of closing network/IPC channels.
        // A later server stop must not erase the fault that caused that stop.
        return fault->code() == Fault::STARTUP_TIMEOUT || fault->code() == Fault::PROTOCOL_REJECTED ||
               fault->stage() == Fault::CAPTURE_ENCODE || fault->stage() == Fault::DISPLAY_RESIZE ||
               fault->stage() == Fault::INPUT_BINDING || fault->stage() == Fault::INPUT_RELEASE ||
               fault->stage() == Fault::INPUT_APPLY || fault->stage() == Fault::INPUT_VALIDATION ||
               fault->stage() == Fault::LIVENESS;
    }
    void applyCancellation() {
        if (!forcedExit && !hasIndependentFailure()) {
            terminal.Clear();
            terminal.mutable_session_close()->set_reason("cancelled");
            fault.reset();
        }
    }
    void recordWorkerExit(std::uint32_t code, std::uint32_t windowsSession) {
        exitCode = code;
        auto worker = protocol::desktopFailureFromExitCode(code, windowsSession, processId);
        // A worker's first fault is authoritative over the resulting pipe loss,
        // but cannot replace an earlier service timeout or network failure.
        if (worker && (!fault || fault->stage() == wire::DesktopFault::SESSION_BRIDGE || fault->stage() == wire::DesktopFault::BRIDGE_WORKER_READ || fault->stage() == wire::DesktopFault::WORKER_PIPE_PAIRING)) {
            fault = std::move(worker);
        }
        if (!fault && (code || forcedExit)) {
            fault.emplace();
            fault->set_stage(wire::DesktopFault::WORKER_EXIT);
            fault->set_code(forcedExit ? wire::DesktopFault::EXIT_TIMEOUT : wire::DesktopFault::PROCESS_EXIT);
            fault->set_windows_session_id(windowsSession);
            fault->set_process_id(processId);
        }
    }
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
