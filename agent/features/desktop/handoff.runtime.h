#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
inline constexpr std::uint32_t kWindowsSessionTerminationExitCode = 0x40010004u;

// A notification observed while selecting a console belongs to the previous
// binding. Commit the selected target and clear that notification atomically
// on the owner Worker, after (not before) the asynchronous selection finishes.
class DesktopHandoffState {
  public:
    explicit DesktopHandoffState(std::uint32_t session) : session_(session) {}
    std::uint32_t session() const { return session_; }
    bool select(std::uint32_t session) {
        if (!session || session == UINT32_MAX) {
            return false;
        }
        session_ = session;
        pending_ = false;
        return true;
    }
    bool observe(std::uint32_t current) {
        if (current == session_) {
            return false;
        }
        pending_ = true;
        return true;
    }
    bool changed(std::uint32_t current) const { return pending_ || current != session_; }
    bool replaceableExit(std::uint32_t current, std::uint32_t exitCode, bool workerStarted, std::optional<wire::DesktopFault::Code> fault) const {
        // Windows terminates processes inside a logging-off session with
        // DBG_TERMINATE_PROCESS (0x40010004) even when the replacement logon
        // desktop reuses the same console session ID.
        if (exitCode == kWindowsSessionTerminationExitCode && !fault) {
            return true;
        }
        if (!changed(current)) {
            return false;
        }
        // Before DesktopStart there has been no capture/input work. Closing its
        // still-unpaired pipe can make the bootstrap return ordinary exit=1.
        return exitCode == 0 || fault == wire::DesktopFault::IPC_FAILED || (!workerStarted && exitCode == 1 && !fault);
    }

  private:
    std::uint32_t session_;
    bool pending_ = false;
};
// A flapping console cannot keep an authorized connection in endless recovery,
// while long-lived sessions can cross normal logon/logoff/lock transitions.
class DesktopHandoffBudget {
  public:
    using Clock = std::chrono::steady_clock;
    bool admit(Clock::time_point now = Clock::now()) {
        if (!attempts_ || now - window_ >= std::chrono::seconds(60)) {
            window_ = now;
            attempts_ = 0;
        }
        if (attempts_ == 8) {
            return false;
        }
        ++attempts_;
        return true;
    }

  private:
    Clock::time_point window_{};
    unsigned attempts_ = 0;
};
} // namespace ultraterimal::agent
