#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

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
        std::uint32_t framesPerSecond = 30;
        std::uint32_t bitrate = 24000000;
        bool sharp = true;
    };
    static constexpr std::uint32_t kMinimumFramesPerSecond = 10;
    static constexpr std::uint32_t kMaximumFramesPerSecond = 30;
    static constexpr std::uint32_t kReferenceFramesPerSecond = 60;
    // Keep the pacing floor aligned with VideoEncoder and SoftwareVideoEncoder.
    static constexpr std::uint32_t kMinimumVideoBitrate = 64000;
    static constexpr std::uint32_t kMaximumVideoBitrate = 100000000;
    static constexpr std::uint32_t kLegacyMaximumInFlightFrames = 16;
    static constexpr std::size_t kMaximumInFlightFrames = 128;

    Rate rate() const { return rate_; }
    bool background() const { return background_; }
    std::pair<std::uint32_t, std::uint32_t> geometry() const { return {width_, height_}; }
    std::uint32_t clearBitrateAt60() const {
        const auto area = static_cast<std::uint64_t>(width_) * height_;
        constexpr std::uint64_t referenceArea = 1920ull * 1080;
        constexpr std::uint64_t referenceBitrate = 8000000;
        const auto bits = (referenceBitrate * area + referenceArea - 1) / referenceArea;
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(bits, kMinimumVideoBitrate, kMaximumVideoBitrate));
    }
    std::uint32_t minimumBitrate(std::uint32_t framesPerSecond) const {
        const auto boundedFps = std::clamp(framesPerSecond, kMinimumFramesPerSecond, kMaximumFramesPerSecond);
        const auto bits = (static_cast<std::uint64_t>(clearBitrateAt60()) * boundedFps + kReferenceFramesPerSecond - 1) / kReferenceFramesPerSecond;
        return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(bits, kMinimumVideoBitrate, kMaximumVideoBitrate));
    }
    // Geometry is discovered/updated by the encoding worker, then committed on
    // this owner loop before another frame is admitted.
    bool setGeometry(std::uint32_t width, std::uint32_t height) {
        const auto area = static_cast<std::uint64_t>(width) * height;
        if (width < 16 || height < 16 || width > 4096 || height > 4096 || (width & 1) || (height & 1) || area > 4096ull * 2160) {
            return false;
        }
        width_ = width;
        height_ = height;
        const auto currentFloor = minimumBitrate(rate_.framesPerSecond);
        const auto healthyTarget = std::max(preferredBitrate(), clearBitrateAt60());
        rate_.bitrate = std::max(currentFloor, std::min(rate_.bitrate, healthyTarget));
        return true;
    }
    void observeRoundTrip(std::chrono::microseconds elapsed, Clock::time_point now) {
        if (elapsed <= std::chrono::microseconds::zero() || elapsed >= std::chrono::seconds(60)) {
            return;
        }
        if (!roundTrip_ || now >= roundTripExpiresAt_) {
            roundTrip_ = elapsed;
            roundTripExpiresAt_ = now + std::chrono::seconds(30);
        } else {
            roundTrip_ = std::min(*roundTrip_, elapsed);
        }
    }
    bool setQuality(Quality quality) {
        if (quality == quality_) {
            return false;
        }
        quality_ = quality;
        rate_ = {kMaximumFramesPerSecond, std::max(preferredBitrate(), clearBitrateAt60()), quality == Quality::kSharp};
        congestionSamples_ = 0;
        healthySince_.reset();
        fpsAdjustAt_ = {};
        return true;
    }
    void reset(std::uint64_t generation) {
        generation_ = generation;
        pending_.clear();
        pendingBytes_ = 0;
        lastAcknowledgedAt_ = {};
        acknowledged_ = 0;
        clientFrameLimit_ = kLegacyMaximumInFlightFrames;
        congestionSamples_ = 0;
        healthySince_.reset();
        fpsAdjustAt_ = {};
    }
    std::size_t frameWindow() const {
        // Use the slower of the independent RTT probe and displayed-frame
        // baseline. Deliberately size the flight at the maximum 30 FPS, not the
        // adapted output rate, so lowering FPS does not contract an in-flight
        // window that is still draining. Keep the legacy 16-frame lower bound,
        // then respect smaller explicit client limits and the hard maximum.
        const auto latency = std::max(roundTrip_.value_or(std::chrono::microseconds::zero()), baseline_);
        const auto microseconds = static_cast<std::uint64_t>(std::clamp(latency, std::chrono::microseconds::zero(), std::chrono::microseconds(std::chrono::seconds(2))).count());
        const auto frames = (microseconds * kMaximumFramesPerSecond + 999999) / 1000000 + kFrameWindowMargin;
        const auto rttWindow = std::clamp<std::uint64_t>(frames, kLegacyMaximumInFlightFrames, kMaximumInFlightFrames);
        return std::min({static_cast<std::size_t>(rttWindow), static_cast<std::size_t>(clientFrameLimit_), kMaximumInFlightFrames});
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
        const auto latency = std::min(std::max(roundTrip_.value_or(std::chrono::microseconds::zero()), baseline_),
                                      std::chrono::microseconds(std::chrono::seconds(2)));
        return pending_.size() < frameWindow() && pendingBytes_ < byteWindow() &&
               now - pending_.front().sent < latency + std::chrono::milliseconds(100);
    }
    std::size_t byteWindow() const {
        if (!hasBaseline_) {
            return 64 * 1024;
        }
        const auto bytesPerSecond = deliveryBytesPerSecond_ ? std::min<std::uint64_t>(rate_.bitrate / 8, deliveryBytesPerSecond_) : rate_.bitrate / 8;
        // Bandwidth-delay product plus 50 ms of queueing, with a hard memory cap.
        const auto latency = std::clamp(std::max(roundTrip_.value_or(std::chrono::microseconds::zero()), baseline_),
                                        std::chrono::microseconds(1000), std::chrono::microseconds(2000000));
        const auto microseconds = static_cast<std::uint64_t>(latency.count()) + 50000;
        return static_cast<std::size_t>(std::clamp<std::uint64_t>(bytesPerSecond * microseconds / 1000000, 32 * 1024, 2 * 1024 * 1024));
    }
    std::size_t pendingBytes() const { return pendingBytes_; }
    std::size_t pendingFrames() const { return pending_.size(); }
    void sent(std::uint64_t generation, std::uint64_t frame, Clock::time_point now, std::size_t bytes) {
        if (generation != generation_) {
            reset(generation);
        }
        // The admission window can shrink while an async encode is in flight;
        // only this immutable safety ceiling is checked at commit time.
        if (pending_.size() >= kMaximumInFlightFrames) {
            throw std::logic_error("Encoded frame exceeded the hard acknowledgement limit");
        }
        if (bytes > std::numeric_limits<std::size_t>::max() - pendingBytes_) {
            throw std::overflow_error("Pending video byte count overflow");
        }
        pending_.push_back({frame, now, bytes});
        pendingBytes_ += bytes;
    }
    bool acknowledge(std::uint64_t generation, std::uint64_t frame, std::uint32_t decodeMicroseconds,
                     std::uint32_t queueDepth, Clock::time_point now, bool background = false,
                     std::uint32_t advertisedMaximumPendingFrames = 0) {
        if (advertisedMaximumPendingFrames > kMaximumInFlightFrames || !frame || generation != generation_ || frame < acknowledged_) {
            return false;
        }
        // Visibility can change while a static desktop produces no new frames.
        // Repeated ACKs may update visibility, but never adjust the rate again.
        if (frame == acknowledged_) {
            clientFrameLimit_ = advertisedMaximumPendingFrames ? advertisedMaximumPendingFrames : kLegacyMaximumInFlightFrames;
            if (background != background_) {
                congestionSamples_ = 0;
                healthySince_.reset();
            }
            background_ = background;
            return true;
        }
        const auto found = std::find_if(pending_.begin(), pending_.end(), [frame](const Pending& value) { return value.frame == frame; });
        if (found == pending_.end() || now < found->sent) {
            return false;
        }
        clientFrameLimit_ = advertisedMaximumPendingFrames ? advertisedMaximumPendingFrames : kLegacyMaximumInFlightFrames;
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - found->sent);
        if (!hasBaseline_ || now >= baselineExpiresAt_) {
            baseline_ = elapsed;
            baselineExpiresAt_ = now + std::chrono::seconds(30);
            hasBaseline_ = true;
        } else {
            baseline_ = std::min(baseline_, elapsed);
        }
        std::size_t delivered = 0;
        for (auto current = pending_.begin(); current != std::next(found); ++current) {
            delivered += current->bytes;
        }
        const auto ackInterval = std::chrono::duration_cast<std::chrono::microseconds>(now - lastAcknowledgedAt_);
        // Only pipelined frames measure delivery capacity; static desktops and
        // time spent idle must not be mistaken for a slow link.
        if (lastAcknowledgedAt_ != Clock::time_point{} && found->sent <= lastAcknowledgedAt_ && ackInterval >= std::chrono::milliseconds(5)) {
            const auto sample = std::min<std::uint64_t>(100000000, delivered * 1000000ull / static_cast<std::uint64_t>(ackInterval.count()));
            deliveryBytesPerSecond_ = deliveryBytesPerSecond_ ? (deliveryBytesPerSecond_ * 3 + sample) / 4 : sample;
        }
        lastAcknowledgedAt_ = now;
        pendingBytes_ -= delivered;
        pending_.erase(pending_.begin(), std::next(found));
        acknowledged_ = frame;
        background_ = background;
        if (background_) {
            congestionSamples_ = 0;
            healthySince_.reset();
            return true;
        }
        // WebCodecs output includes browser scheduling jitter, so a single
        // frame slightly over its nominal interval is not decoder overload.
        const bool decodeBound = decodeMicroseconds > 50000;
        const auto networkElapsed = elapsed - std::min(elapsed, std::chrono::microseconds(decodeMicroseconds));
        const auto networkBaseline = roundTrip_.value_or(baseline_);
        const bool transportBound = networkElapsed > networkBaseline + std::chrono::milliseconds(40) || queueDepth > 2;
        const bool congested = transportBound || decodeBound;
        if (congested) {
            healthySince_.reset();
        } else if (!healthySince_) {
            healthySince_ = now;
        }
        // Browser scheduling and one-off decoder stalls are noisy. Observe
        // every new ACK, including the cooldown, before lowering the rate.
        congestionSamples_ = congested ? std::min(3u, congestionSamples_ + 1) : 0;
        if (now < adjustAt_ || (congested && congestionSamples_ < 3)) {
            return true;
        }
        if (congested) {
            if (decodeBound && now >= fpsAdjustAt_) {
                rate_.framesPerSecond = std::max(kMinimumFramesPerSecond, rate_.framesPerSecond > kMinimumFramesPerSecond + 3 ? rate_.framesPerSecond - 3 : kMinimumFramesPerSecond);
                fpsAdjustAt_ = now + std::chrono::seconds(2);
            }
            if (transportBound) {
                reduceForNetworkCongestion(networkElapsed, delivered);
            }
            congestionSamples_ = 0;
        } else {
            recoverRate();
            // Wait for sustained healthy display feedback before raising FPS;
            // bitrate recovery alone must not make the visible cadence oscillate.
            if (healthySince_ && now - *healthySince_ >= std::chrono::seconds(5) && now >= fpsAdjustAt_) {
                const auto nextFps = std::min(kMaximumFramesPerSecond, rate_.framesPerSecond + 3);
                if (rate_.bitrate >= minimumBitrate(nextFps)) {
                    rate_.framesPerSecond = nextFps;
                    fpsAdjustAt_ = now + std::chrono::seconds(5);
                    healthySince_ = now;
                }
            }
        }
        adjustAt_ = now + std::chrono::milliseconds(500);
        return true;
    }

  private:
    static constexpr std::uint64_t kFrameWindowMargin = 4;
    struct Pending {
        std::uint64_t frame;
        Clock::time_point sent;
        std::size_t bytes;
    };
    std::uint32_t preferredBitrate() const {
        return quality_ == Quality::kSharp ? 24000000u : quality_ == Quality::kSmooth ? 8000000u
                                                                                      : 16000000u;
    }
    std::uint64_t availableBitrate(std::chrono::microseconds networkElapsed, std::size_t delivered) const {
        std::uint64_t available = deliveryBytesPerSecond_ ? deliveryBytesPerSecond_ * 8 * 9 / 10 : kMaximumVideoBitrate;
        if (roundTrip_ && networkElapsed > *roundTrip_ + std::chrono::milliseconds(40)) {
            const auto deliveryTime = static_cast<std::uint64_t>((networkElapsed - *roundTrip_).count());
            const auto observed = static_cast<std::uint64_t>(delivered) * 8000000ull / deliveryTime * 9 / 10;
            available = std::min(available, observed);
        }
        return available;
    }
    void reduceForNetworkCongestion(std::chrono::microseconds networkElapsed, std::size_t delivered) {
        auto target = static_cast<std::uint64_t>(rate_.bitrate) * 3 / 4;
        target = std::min(target, availableBitrate(networkElapsed, delivered));
        target = std::min<std::uint64_t>(target, kMaximumVideoBitrate);
        // Network estimates are not a reliable measure of maximum capacity when
        // the application is frame- or content-limited. Lower bitrate only as
        // far as the current per-frame clarity budget; bounded ACK/byte/age
        // admission limits actual throughput on a narrower path.
        target = std::max<std::uint64_t>(minimumBitrate(rate_.framesPerSecond), target);
        rate_.bitrate = static_cast<std::uint32_t>(target);
    }
    void recoverRate() {
        const auto target = std::max(preferredBitrate(), clearBitrateAt60());
        const auto increment = std::max(64000u, rate_.bitrate / 10);
        rate_.bitrate = std::min(target, rate_.bitrate > kMaximumVideoBitrate - increment ? kMaximumVideoBitrate : rate_.bitrate + increment);
    }

    std::uint32_t width_ = 1920, height_ = 1080;
    Rate rate_;
    Quality quality_ = Quality::kSharp;
    std::uint32_t clientFrameLimit_ = kLegacyMaximumInFlightFrames;
    std::deque<Pending> pending_;
    std::uint64_t generation_ = 0, acknowledged_ = 0;
    std::size_t pendingBytes_ = 0;
    std::uint64_t deliveryBytesPerSecond_ = 0;
    bool hasBaseline_ = false;
    std::chrono::microseconds baseline_{250000};
    Clock::time_point baselineExpiresAt_{}, lastAcknowledgedAt_{};
    std::optional<std::chrono::microseconds> roundTrip_;
    Clock::time_point roundTripExpiresAt_{};
    Clock::time_point adjustAt_{}, fpsAdjustAt_{};
    std::optional<Clock::time_point> healthySince_;
    bool background_ = false;
    unsigned congestionSamples_ = 0;
};
} // namespace ultraterimal::agent
