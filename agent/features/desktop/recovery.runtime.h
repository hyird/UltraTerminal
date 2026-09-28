#pragma once
#include <chrono>

namespace ultraterimal::agent {
// At most three capture restarts within ten seconds. A successful frame does
// not reset the budget: a flapping device must not restart forever.
class CaptureRecovery {
  public:
    using Clock = std::chrono::steady_clock;
    bool restart(Clock::time_point now) {
        if (!attempts_ || now - window_ >= std::chrono::seconds(10)) {
            window_ = now;
            attempts_ = 0;
        }
        if (attempts_ == 3) {
            return false;
        }
        ++attempts_;
        next_ = now + std::chrono::milliseconds(500U << (attempts_ - 1));
        return true;
    }
    bool ready(Clock::time_point now) const { return now >= next_; }
    Clock::time_point nextAttempt() const { return next_; }

  private:
    Clock::time_point window_{}, next_{};
    unsigned attempts_ = 0;
};
} // namespace ultraterimal::agent
