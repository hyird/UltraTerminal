#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Owned by the control worker. In-flight entries remain owned until acknowledged.
class AgentLogBuffer {
  public:
    explicit AgentLogBuffer(std::string runtimeId) { batch_.set_runtime_id(std::move(runtimeId)); }
    void append(wire::AgentLog::Event event, std::uint64_t timestamp, std::string sessionId = {}, std::uint32_t exitCode = 0, bool forcedExit = false, const wire::DesktopFault* fault = nullptr) {
        if (pending_.size() >= kCapacity) {
            pending_.pop_front();
            ++dropped_;
        }
        wire::AgentLog entry;
        entry.set_sequence(++sequence_);
        entry.set_unix_milliseconds(timestamp);
        entry.set_event(event);
        entry.set_session_id(std::move(sessionId));
        entry.set_exit_code(exitCode);
        entry.set_forced_exit(forcedExit);
        if (fault && event == wire::AgentLog::DESKTOP_FAILED) {
            *entry.mutable_desktop_fault() = *fault;
        }
        pending_.push_back(std::move(entry));
    }
    const wire::AgentLogBatch* next() {
        if (batch_.entries().empty()) {
            while (!pending_.empty() && batch_.entries_size() < 16) {
                *batch_.add_entries() = std::move(pending_.front());
                pending_.pop_front();
            }
            batch_.set_dropped(std::exchange(dropped_, 0));
        }
        return batch_.entries().empty() ? nullptr : &batch_;
    }
    bool acknowledge(std::uint64_t sequence) {
        if (batch_.entries().empty() || batch_.entries().rbegin()->sequence() != sequence) {
            return false;
        }
        batch_.clear_entries();
        batch_.set_dropped(0);
        return true;
    }
    static constexpr std::size_t kCapacity = 256;

  private:
    std::deque<wire::AgentLog> pending_;
    wire::AgentLogBatch batch_;
    std::uint64_t sequence_ = 0, dropped_ = 0;
};
} // namespace ultraterimal::agent
