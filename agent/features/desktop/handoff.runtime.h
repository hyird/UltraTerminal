#pragma once

#include <cstdint>
#include <optional>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
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
// A flapping console cannot keep an authorized connection in endless recovery.
// Successful frames and elapsed time do not replenish this connection's budget.
class DesktopHandoffBudget {
  public:
    bool admit() {
        if (attempts_ == 3) {
            return false;
        }
        ++attempts_;
        return true;
    }

  private:
    unsigned attempts_ = 0;
};
} // namespace ultraterimal::agent
