#pragma once
#include <chrono>

namespace ultraterimal::agent {
// At most ten capture restarts within fifteen seconds. Backoff ramps from
// 250ms to 1000ms to bridge Windows logon/logoff and WDDM transitions without
// dropping the remote session, while keeping recovery strictly bounded.
class CaptureRecovery {
  public:
    using Clock = std::chrono::steady_clock;
    bool restart(Clock::time_point now) {
        if (!attempts_ || now - window_ >= std::chrono::seconds(15)) {
            window_ = now;
            attempts_ = 0;
        }
        if (attempts_ == 10) {
            return false;
        }
        ++attempts_;
        const unsigned shift = attempts_ <= 1 ? 0u : (attempts_ == 2 ? 1u : 2u);
        next_ = now + std::chrono::milliseconds(250U << shift);
        return true;
    }
    bool ready(Clock::time_point now) const { return now >= next_; }
    Clock::time_point nextAttempt() const { return next_; }

  private:
    Clock::time_point window_{}, next_{};
    unsigned attempts_ = 0;
};
} // namespace ultraterimal::agent
