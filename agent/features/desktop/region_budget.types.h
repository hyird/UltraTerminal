#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace ultraterimal::agent {
// Admission budget, not cancellation of a synchronous system codec call.
// A slow encoder is reprobed at most once per second; cache hits need no probe.
class DesktopRegionBudget {
  public:
    using Clock = std::chrono::steady_clock;
    void begin(Clock::time_point now, std::uint32_t framesPerSecond) {
        if (!framesPerSecond || framesPerSecond > 120) {
            throw std::invalid_argument("Invalid region frame rate");
        }
        deadline_ = now + std::chrono::microseconds(std::clamp(250000u / framesPerSecond, 1000u, 4000u));
        denied_ = false;
    }
    bool within(Clock::time_point now) const { return now < deadline_; }
    bool canEncode(Clock::time_point now) {
        const bool allowed = within(now) && (now + estimate_ <= deadline_ || !lastEncode_ || now - *lastEncode_ >= std::chrono::seconds(1));
        denied_ |= !allowed;
        return allowed;
    }
    bool denied() const { return denied_; }
    void observe(std::chrono::microseconds elapsed, Clock::time_point now) {
        estimate_ = std::clamp(elapsed + elapsed / 4, std::chrono::microseconds(100), std::chrono::microseconds(200000));
        lastEncode_ = now;
    }

  private:
    Clock::time_point deadline_{};
    std::optional<Clock::time_point> lastEncode_;
    std::chrono::microseconds estimate_{250};
    bool denied_ = false;
};
} // namespace ultraterimal::agent
