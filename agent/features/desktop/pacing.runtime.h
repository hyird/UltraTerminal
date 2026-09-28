#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <stdexcept>

namespace ultraterimal::agent {
// Event-loop owned. Display acknowledgements measure the complete path rather
// than completion of a write into the local TCP buffer.
class DesktopPacing {
  public:
    using Clock = std::chrono::steady_clock;
    enum class Quality { kBalanced,
                         kSmooth,
                         kSharp };
    struct Rate {
        std::uint32_t framesPerSecond = 60;
        std::uint32_t bitrate = 24000000;
        bool sharp = true;
    };
    Rate rate() const { return rate_; }
    bool background() const { return background_; }
    bool setQuality(Quality quality) {
        if (quality == quality_) {
            return false;
        }
        quality_ = quality;
        rate_ = {60, quality == Quality::kSharp ? 24000000u : quality == Quality::kSmooth ? 8000000u
                                                                                          : 16000000u};
        congestionSamples_ = 0;
        rate_.sharp = quality == Quality::kSharp;
        return true;
    }
    void reset(std::uint64_t generation) {
        generation_ = generation;
        pending_.clear();
        acknowledged_ = 0;
        congestionSamples_ = 0;
    }
    bool canEncode(Clock::time_point now) const {
        if (background_) {
            return false;
        }
        if (pending_.empty()) {
            return true;
        }
        // Bound both count and age. Capture may continue to retain the newest
        // pixels and cursor, but no dependent encoded frame is discarded.
        return pending_.size() < 16 && now - pending_.front().sent < baseline_ + std::chrono::milliseconds(100);
    }
    void sent(std::uint64_t generation, std::uint64_t frame, Clock::time_point now) {
        if (generation != generation_) {
            reset(generation);
        }
        if (pending_.size() == 16) {
            throw std::logic_error("Encoded frame exceeded the acknowledgement window");
        }
        pending_.push_back({frame, now});
    }
    bool acknowledge(std::uint64_t generation, std::uint64_t frame, std::uint32_t decodeMicroseconds,
                     std::uint32_t queueDepth, Clock::time_point now, bool background = false) {
        if (!frame || generation != generation_ || frame < acknowledged_) {
            return false;
        }
        // Visibility can change while a static desktop produces no new frames.
        // Repeated ACKs may update visibility, but never adjust the rate again.
        if (frame == acknowledged_) {
            if (background != background_) {
                congestionSamples_ = 0;
            }
            background_ = background;
            return true;
        }
        const auto found = std::find_if(pending_.begin(), pending_.end(), [frame](const Pending& value) { return value.frame == frame; });
        if (found == pending_.end() || now < found->sent) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - found->sent);
        baseline_ = std::min(baseline_, elapsed);
        pending_.erase(pending_.begin(), std::next(found));
        acknowledged_ = frame;
        background_ = background;
        if (background_) {
            congestionSamples_ = 0;
            return true;
        }
        // WebCodecs output includes browser scheduling jitter, so a single
        // frame slightly over its nominal interval is not decoder overload.
        // Sustained 50 ms output latency is slow enough to justify reducing
        // temporal load; network delay is handled separately by bitrate and
        // the bounded acknowledgement window.
        const bool decodeBound = decodeMicroseconds > 50000;
        const bool transportBound = elapsed > baseline_ + std::chrono::milliseconds(40) || queueDepth > 2;
        const bool congested = transportBound || decodeBound;
        // Browser scheduling and one-off decoder stalls are noisy. Observe
        // every new ACK, including the cooldown, before lowering the rate.
        congestionSamples_ = congested ? std::min(3u, congestionSamples_ + 1) : 0;
        if (now < adjustAt_ || (congested && congestionSamples_ < 3)) {
            return true;
        }
        if (congested) {
            // Quality preference is a healthy-link target, not a bandwidth floor.
            // Preserve responsiveness on sub-megabit links instead of accumulating
            // frames indefinitely at the former 8 Mbps sharp-mode minimum.
            constexpr auto minimumBitrate = 512000u;
            // Network pressure is relieved by reducing bytes while the ACK
            // window supplies backpressure. It must not turn harmless RTT or
            // a low-byte desktop update into a persistent low frame rate.
            if (decodeBound) {
                rate_.framesPerSecond = std::max(10u, rate_.framesPerSecond * 3 / 4);
            }
            if (transportBound && rate_.bitrate > minimumBitrate) {
                rate_.bitrate = std::max(minimumBitrate, rate_.bitrate * 3 / 4);
            }
            congestionSamples_ = 0;
        } else {
            rate_.framesPerSecond = std::min(60u, rate_.framesPerSecond + 5);
            const auto preferredBitrate = quality_ == Quality::kSharp ? 24000000u : quality_ == Quality::kSmooth ? 8000000u
                                                                                                                 : 16000000u;
            rate_.bitrate = std::min(preferredBitrate, rate_.bitrate + std::max(64000u, rate_.bitrate / 10));
        }
        adjustAt_ = now + std::chrono::milliseconds(500);
        return true;
    }

  private:
    struct Pending {
        std::uint64_t frame;
        Clock::time_point sent;
    };
    Rate rate_;
    Quality quality_ = Quality::kSharp;
    std::deque<Pending> pending_;
    std::uint64_t generation_ = 0, acknowledged_ = 0;
    std::chrono::microseconds baseline_{30000000};
    Clock::time_point adjustAt_{};
    bool background_ = false;
    unsigned congestionSamples_ = 0;
};
} // namespace ultraterimal::agent
