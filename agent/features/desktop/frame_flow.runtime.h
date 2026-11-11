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
// Owner-loop receiver credit. Encoded dependent frames are never discarded.
// Frame ACKs release credit; RTT probes and byte-saturated ACK delivery estimate
// path capacity. Unsaturated offered load is not a capacity ceiling.
class DesktopFrameFlow {
  public:
    using Clock = std::chrono::steady_clock;
    enum class FrameType { kVideo,
                           kKeyFrame,
                           kRepeatedVideo,
                           kRepeatedKeyFrame };
    struct Rate {
        std::uint32_t framesPerSecond = 30;
        std::uint32_t bitrate = kTargetVideoBitrate;
    };
    static constexpr std::size_t kMaximumInFlightFrames = 128;
    // Clarity budget, encode/decode cost, receiver credit and congestion still
    // decide the actual cadence below this ceiling.
    static constexpr std::uint32_t kMinimumFramesPerSecond = 5, kMaximumFramesPerSecond = 60;
    static constexpr std::uint32_t kMinimumVideoBitrate = 64000, kMaximumVideoBitrate = 100000000;
    static constexpr std::uint32_t kTargetVideoBitrate = 32000000;
    static constexpr std::uint32_t kLegacyMaximumInFlightFrames = 16;
    Rate rate() const { return rate_; }
    bool background() const { return background_; }
    std::pair<std::uint32_t, std::uint32_t> geometry() const { return {width_, height_}; }
    bool setGeometry(std::uint32_t width, std::uint32_t height) {
        if (width < 16 || height < 16 || width > 4096 || height > 4096 || (width & 1) || (height & 1) || static_cast<std::uint64_t>(width) * height > 4096ull * 2160) {
            return false;
        }
        width_ = width;
        height_ = height;
        scenePixels_.reset();
        videoFrameBytes_ = videoPacingBytes_ = 0;
        videoPacingActive_ = false;
        videoBudgetFrames_ = 0;
        updateFrameRate();
        return true;
    }
    // V2 encoders obey VBV: a burst only redistributes bytes and its real
    // size is already bounded by byte credit. Only a quality-floor encoder
    // (legacy sharp QP bounds) has a fixed picture cost that bounds cadence.
    void setFixedPictureCost(bool fixed) {
        if (fixed == fixedPictureCost_) {
            return;
        }
        fixedPictureCost_ = fixed;
        videoPacingBytes_ = 0;
        videoBudgetFrames_ = 0;
    }
    void reset(std::uint64_t generation) {
        generation_ = generation;
        pending_.clear();
        pendingBytes_ = 0;
        acknowledged_ = lastSent_ = 0;
        lastContentFrame_ = 0;
        lastAcknowledgedAt_ = {};
        baseline_.reset();
        deliveryBytesPerSecond_ = 0;
        videoFrameBytes_ = videoPacingBytes_ = 0;
        videoPacingActive_ = false;
        videoBudgetFrames_ = 0;
        clientFrameLimit_ = kLegacyMaximumInFlightFrames;
        receiverFrameRate_ = kMaximumFramesPerSecond;
        decodeMicroseconds_ = 0;
        scenePixels_.reset();
        congestionSamples_ = 0;
        healthySince_.reset();
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
    void observeFrame(FrameType type, std::size_t bytes, std::uint64_t changedPixels, std::chrono::microseconds elapsed) {
        if (keyFrame(type)) {
            videoBudgetFrames_ = 0;
            return; // Codec startup and refresh bursts do not describe steady frame cost.
        }
        if (repeatedFrame(type)) {
            videoBudgetFrames_ = 0;
            return; // Delivery aliases cannot erase the next changed picture's clarity/cost budget.
        }
        if (elapsed.count() > 0) {
            encodeMicroseconds_ = encodeMicroseconds_ ? (encodeMicroseconds_ * 7 + static_cast<std::uint64_t>(elapsed.count())) / 8 : static_cast<std::uint64_t>(elapsed.count());
        }
        const auto pixels = std::min(changedPixels, static_cast<std::uint64_t>(width_) * height_);
        scenePixels_ = static_cast<std::uint64_t>(width_) * height_;
        // Ordinary changed pictures size the existing two-picture credit cushion,
        // even when sharp's quality floor exceeds VBV. Keyframes, repeats and
        // static snapshots must not inflate or dilute this steady-frame estimate.
        videoPacingActive_ = type == FrameType::kVideo && pixels != 0;
        if (videoPacingActive_) {
            const auto sample = std::min<std::uint64_t>(bytes, 4 * 1024 * 1024);
            videoFrameBytes_ = videoFrameBytes_ ? (videoFrameBytes_ * 7 + sample) / 8 : sample;
            const auto nominalBytes = static_cast<std::uint64_t>(rate_.bitrate) / (8 * rate_.framesPerSecond);
            // The observed wire picture includes metadata and modest VBV variation.
            // This tolerance validates a cost probe; it never enlarges credit.
            const auto budgetTolerance = std::max<std::uint64_t>(512, nominalBytes / 20);
            // Only a quality-floor encoder learns a payload floor, from any real
            // overrun of its per-picture budget: those bytes do not shrink with
            // cadence. VBV pictures (setFixedPictureCost(false)) redistribute
            // bytes as FPS changes and never learn one. A throttled cadence can
            // make a revalidated picture look as expensive as the old one. Four
            // budget-compliant changed pictures permit a bounded +1 FPS
            // revalidation, not an unconditional floor reset. If the next
            // picture exceeds that new budget, immediately restore its real cost.
            if (fixedPictureCost_ && (sample > nominalBytes + budgetTolerance || (videoPacingBytes_ && sample * 4 >= videoPacingBytes_ * 3))) {
                if (sample > nominalBytes + budgetTolerance) {
                    videoPacingBytes_ = sample;
                    videoBudgetFrames_ = 0;
                } else {
                    videoPacingBytes_ = std::min(nominalBytes, (videoPacingBytes_ * 7 + sample) / 8);
                    videoBudgetFrames_ = std::min(kVideoCostProbeFrames, videoBudgetFrames_ + 1);
                    const auto nextFps = rate_.framesPerSecond + 1;
                    if (videoBudgetFrames_ == kVideoCostProbeFrames && !background_ && !congestionSamples_ && nextFps <= receiverFrameRate_ &&
                        (!encodeMicroseconds_ || nextFps <= 800000ull / encodeMicroseconds_) &&
                        (decodeMicroseconds_ <= 25000 || nextFps <= 800000ull / decodeMicroseconds_)) {
                        videoPacingBytes_ = std::min(videoPacingBytes_, static_cast<std::uint64_t>(rate_.bitrate) / (8 * nextFps));
                        videoBudgetFrames_ = 0;
                    }
                }
            } else {
                videoPacingBytes_ = 0;
                videoBudgetFrames_ = 0;
            }
        } else {
            videoBudgetFrames_ = 0;
        }
        // Capacity and codec/receiver pressure still bound this increase.
        updateFrameRate(true, 4);
    }
    std::size_t frameWindow() const {
        if (!baseline_ && !roundTrip_) {
            return std::min<std::size_t>(2, clientFrameLimit_);
        }
        const auto frames = (static_cast<std::uint64_t>(pathDelay().count()) * rate_.framesPerSecond + 999999) / 1000000 + 2;
        return std::min<std::size_t>(std::clamp<std::uint64_t>(frames, 2, kMaximumInFlightFrames), clientFrameLimit_);
    }
    std::size_t byteWindow() const {
        const auto delivery = deliveryBytesPerSecond_ ? std::min<std::uint64_t>(deliveryBytesPerSecond_, rate_.bitrate / 8) : rate_.bitrate / 8;
        const auto bytes = delivery * static_cast<std::uint64_t>(pathDelay().count() + 50000) / 1000000;
        // ACK delay and codec-size variation need a normal two-picture
        // cushion, just as frameWindow has two slots beyond the path BDP.
        // Keyframes and repeats cannot inflate it; age/frame guards remain.
        return static_cast<std::size_t>(std::clamp<std::uint64_t>(std::max(bytes, videoFrameBytes_ * 2), 16384, 4 * 1024 * 1024));
    }
    bool canEncode(Clock::time_point now) const {
        return !background_ && (pending_.empty() || (pending_.size() < frameWindow() && pressureBytes(now) < byteWindow() && now - pending_.front().sent <= pathDelay() + std::chrono::milliseconds(150)));
    }
    std::size_t pendingBytes() const { return pendingBytes_; }
    std::size_t pendingFrames() const { return pending_.size(); }
    struct Snapshot {
        std::uint32_t frameWindow, byteWindow, pendingFrames, pendingBytes, pressureBytes, pathDelayMicroseconds, oldestFrameAgeMicroseconds, deliveryBytesPerSecond, blockingFlags;
    };
    Snapshot snapshot(Clock::time_point now) const {
        const auto age = pending_.empty() ? Clock::duration::zero() : std::max(now - pending_.front().sent, Clock::duration::zero());
        const auto bytes = pressureBytes(now);
        const auto flags = (background_ ? 1u : 0u) | (!pending_.empty() && pending_.size() >= frameWindow() ? 2u : 0u) |
                           (!pending_.empty() && bytes >= byteWindow() ? 4u : 0u) | (!pending_.empty() && age > pathDelay() + std::chrono::milliseconds(150) ? 8u : 0u);
        return {static_cast<std::uint32_t>(frameWindow()), static_cast<std::uint32_t>(byteWindow()), static_cast<std::uint32_t>(pending_.size()),
                static_cast<std::uint32_t>(pendingBytes_), static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(pathDelay().count()),
                static_cast<std::uint32_t>(std::min<std::int64_t>(60000000, std::chrono::duration_cast<std::chrono::microseconds>(age).count())),
                static_cast<std::uint32_t>(deliveryBytesPerSecond_), flags};
    }
    std::size_t pressureBytes(Clock::time_point) const { return pendingBytes_; }
    void sent(std::uint64_t generation, std::uint64_t frame, Clock::time_point now, std::size_t bytes, FrameType type) {
        if (generation != generation_) {
            if (!generation || generation < generation_) {
                throw std::logic_error("Retired desktop generation sent");
            }
            reset(generation);
        }
        if (!frame || frame <= lastSent_ || pending_.size() >= kMaximumInFlightFrames || bytes > std::numeric_limits<std::size_t>::max() - pendingBytes_) {
            throw std::logic_error("Desktop frame exceeded receiver-credit safety bounds");
        }
        if (repeatedFrame(type) && !lastContentFrame_) {
            throw std::logic_error("Repeated desktop frame has no content baseline");
        }

        if (!repeatedFrame(type)) {
            lastContentFrame_ = frame;
        }
        pending_.push_back({frame, now, bytes, type});
        pendingBytes_ += bytes;
        lastSent_ = frame;
    }
    bool acknowledge(std::uint64_t generation, std::uint64_t frame, std::uint32_t decodeMicroseconds,
                     std::uint32_t queueDepth, Clock::time_point now, bool background = false, std::uint32_t advertisedMaximumPendingFrames = 0) {
        if (generation != generation_ || !frame || frame < acknowledged_ || advertisedMaximumPendingFrames > kMaximumInFlightFrames) {
            return false;
        }
        if (frame == acknowledged_) {
            setReceiver(background, advertisedMaximumPendingFrames);
            return true;
        }
        const auto found = std::find_if(pending_.begin(), pending_.end(), [frame](const Pending& value) { return value.frame == frame; });
        if (found == pending_.end() || now < found->sent) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - found->sent);
        const bool decodedKeyFrame = keyFrame(found->type);
        const auto transit = elapsed - std::min(elapsed, std::chrono::microseconds(decodeMicroseconds));
        if (!baseline_ || now >= baselineExpiresAt_) {
            baseline_ = transit;
            baselineExpiresAt_ = now + std::chrono::seconds(30);
        } else {
            baseline_ = std::min(*baseline_, transit);
        }
        std::size_t delivered = 0;
        bool ordinaryDelivery = true;
        for (auto current = pending_.begin(); current != std::next(found); ++current) {
            delivered += current->bytes;
            ordinaryDelivery = ordinaryDelivery && !keyFrame(current->type) && !repeatedFrame(current->type);
        }
        std::uint64_t ordinaryFlightBytes = 0;
        for (const auto& pending : pending_) {
            if (!keyFrame(pending.type) && !repeatedFrame(pending.type)) {
                ordinaryFlightBytes += pending.bytes;
            }
        }
        // RTT overlap alone is not bandwidth saturation: a stream of 1KB cache
        // Frames can have several outstanding ACKs on an idle link.
        constexpr std::uint64_t minimumCapacityBytes = 16 * 1024;
        const auto pathBytes = std::max(minimumCapacityBytes, static_cast<std::uint64_t>(rate_.bitrate) * pathDelay().count() / 8000000);
        const bool byteSaturated = ordinaryFlightBytes >= pathBytes;
        const auto interval = std::chrono::duration_cast<std::chrono::microseconds>(now - lastAcknowledgedAt_);
        const bool capacitySample = ordinaryDelivery && !background_ && !background && decodeMicroseconds <= 25000 && queueDepth <= 2 &&
                                    lastAcknowledgedAt_ != Clock::time_point{} && found->sent <= lastAcknowledgedAt_ && interval >= std::chrono::milliseconds(5);
        if (capacitySample) {
            const auto sample = std::min<std::uint64_t>(100000000, static_cast<std::uint64_t>(delivered) * 1000000 / static_cast<std::uint64_t>(interval.count()));
            if (sample > deliveryBytesPerSecond_ && (byteSaturated || deliveryBytesPerSecond_)) {
                // Achieved delivery is a lower bound: let it promptly replace a
                // stale narrow-path estimate when actual capacity recovers.
                deliveryBytesPerSecond_ = sample;
            } else if (byteSaturated) {
                deliveryBytesPerSecond_ = (deliveryBytesPerSecond_ * 3 + sample) / 4;
            }
        }
        pendingBytes_ -= delivered;
        pending_.erase(pending_.begin(), std::next(found));
        acknowledged_ = frame;
        lastAcknowledgedAt_ = now;
        setReceiver(background, advertisedMaximumPendingFrames);
        if (background_) {
            return true;
        }
        if (!decodedKeyFrame) {
            decodeMicroseconds_ = decodeMicroseconds_ ? (decodeMicroseconds_ * 7 + decodeMicroseconds) / 8 : decodeMicroseconds;
        }
        // Display/compositor jitter adds ACK delay without saturating the path.
        // Require an ordinary byte-saturated flight before reducing bitrate;
        // frame/byte credit and frame age still bound unsaturated delayed work.
        const bool networkPressure = ordinaryDelivery && byteSaturated && transit > pathDelay() + std::chrono::milliseconds(50);
        const bool decoderPressure = queueDepth > 2 || decodeMicroseconds_ > 25000;
        const bool congested = networkPressure || decoderPressure;
        congestionSamples_ = congested ? std::min(3u, congestionSamples_ + 1) : 0;
        if (congested) {
            videoBudgetFrames_ = 0;
            healthySince_.reset();
        } else if (!healthySince_) {
            healthySince_ = now;
        }
        if (now >= adjustAt_ && congestionSamples_ >= 3) {
            const bool failedRestore = restoredAt_ && now - *restoredAt_ < std::chrono::seconds(5);
            if (!recoveryRate_) {
                recoveryRate_ = rate_;
            } else if (failedRestore) {
                // A failed probe pauses fast restoration without permanently
                // erasing the bounded pre-congestion capacity ceiling.
                restoreBlockedUntil_ = now + std::chrono::seconds(5);
            }
            recoveryExpiresAt_ = now + std::chrono::seconds(30);
            restoredAt_.reset();
            if (networkPressure) {
                auto target = static_cast<std::uint64_t>(rate_.bitrate) * 4 / 5;
                if (capacitySample && byteSaturated && deliveryBytesPerSecond_) {
                    target = std::min(target, deliveryBytesPerSecond_ * 8 * 9 / 10);
                }
                rate_.bitrate = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(target, kMinimumVideoBitrate, kTargetVideoBitrate));
            }
            if (decoderPressure) {
                receiverFrameRate_ = std::max(kMinimumFramesPerSecond, rate_.framesPerSecond * 4 / 5);
            }
            updateFrameRate();
            congestionSamples_ = 0;
            adjustAt_ = now + std::chrono::milliseconds(500);
        } else if (now >= adjustAt_ && healthySince_ && now - *healthySince_ >= std::chrono::seconds(2)) {
            if (recoveryRate_ && now >= recoveryExpiresAt_) {
                recoveryRate_.reset();
            }
            const auto bitrateLimit = recoveryRate_ ? std::min(kTargetVideoBitrate, recoveryRate_->bitrate) : kTargetVideoBitrate;
            const bool restoring = recoveryRate_ && now >= restoreBlockedUntil_;
            const auto step = restoring ? std::max(4u, rate_.framesPerSecond / 4) : 2u;
            rate_.bitrate = std::min(bitrateLimit, rate_.bitrate + std::max(64000u, rate_.bitrate / (restoring ? 4 : 20)));
            receiverFrameRate_ = std::min(kMaximumFramesPerSecond, receiverFrameRate_ + step);
            updateFrameRate(true, step);
            if (restoring) {
                restoredAt_ = now;
            }
            if (recoveryRate_ && rate_.bitrate >= bitrateLimit && rate_.framesPerSecond >= recoveryRate_->framesPerSecond) {
                recoveryRate_.reset();
                restoredAt_ = now;
            }
            adjustAt_ = now + std::chrono::seconds(1);
        }
        return true;
    }

  private:
    static constexpr std::uint32_t kVideoCostProbeFrames = 4;
    static bool keyFrame(FrameType type) { return type == FrameType::kKeyFrame || type == FrameType::kRepeatedKeyFrame; }
    static bool repeatedFrame(FrameType type) { return type == FrameType::kRepeatedVideo || type == FrameType::kRepeatedKeyFrame; }
    struct Pending {
        std::uint64_t frame;
        Clock::time_point sent;
        std::size_t bytes;
        FrameType type;
    };
    std::chrono::microseconds pathDelay() const {
        return std::clamp(roundTrip_.value_or(baseline_.value_or(std::chrono::microseconds(100000))), std::chrono::microseconds(1000), std::chrono::microseconds(2000000));
    }
    void setReceiver(bool background, std::uint32_t capacity) {
        clientFrameLimit_ = capacity ? capacity : kLegacyMaximumInFlightFrames;
        if (background != background_) {
            videoBudgetFrames_ = 0;
            congestionSamples_ = 0;
            healthySince_.reset();
            lastAcknowledgedAt_ = {};
        }
        background_ = background;
    }
    void updateFrameRate(bool recover = false, std::uint32_t recoveryStep = 2) {
        // Prefer fewer readable frames on a narrow path. The bitrate remains
        // bounded by capacity even below the per-frame clarity budget.
        const auto pixels = scenePixels_.value_or(static_cast<std::uint64_t>(width_) * height_);
        const auto clarityBits = std::max<std::uint64_t>(1024, 8000000ull * pixels / (1920ull * 1080 * 60));
        const auto bitsPerFrame = std::max({clarityBits, videoPacingActive_ ? videoPacingBytes_ * 8 : std::uint64_t{0}});
        auto fps = std::min<std::uint64_t>(receiverFrameRate_, rate_.bitrate / bitsPerFrame);
        if (encodeMicroseconds_) {
            fps = std::min<std::uint64_t>(fps, 800000ull / encodeMicroseconds_);
        }
        if (decodeMicroseconds_ > 25000) {
            fps = std::min<std::uint64_t>(fps, 800000ull / decodeMicroseconds_);
        }
        const auto target = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(fps, kMinimumFramesPerSecond, kMaximumFramesPerSecond));
        rate_.framesPerSecond = target < rate_.framesPerSecond ? target : recover ? std::min(target, rate_.framesPerSecond + recoveryStep)
                                                                                  : rate_.framesPerSecond;
    }
    Rate rate_;
    std::uint32_t width_ = 1920, height_ = 1080, clientFrameLimit_ = kLegacyMaximumInFlightFrames;
    std::uint32_t receiverFrameRate_ = kMaximumFramesPerSecond;
    std::uint64_t generation_ = 0, acknowledged_ = 0, lastSent_ = 0;
    std::uint64_t lastContentFrame_ = 0;
    std::uint64_t deliveryBytesPerSecond_ = 0, videoFrameBytes_ = 0, encodeMicroseconds_ = 0, decodeMicroseconds_ = 0;
    std::optional<std::uint64_t> scenePixels_;
    std::uint64_t videoPacingBytes_ = 0;
    bool videoPacingActive_ = false, fixedPictureCost_ = true;
    std::uint32_t videoBudgetFrames_ = 0;
    std::deque<Pending> pending_;
    std::size_t pendingBytes_ = 0;
    std::optional<std::chrono::microseconds> roundTrip_, baseline_;
    Clock::time_point roundTripExpiresAt_{}, baselineExpiresAt_{}, lastAcknowledgedAt_{}, adjustAt_{};
    std::optional<Clock::time_point> healthySince_;
    std::optional<Rate> recoveryRate_;
    std::optional<Clock::time_point> restoredAt_;
    Clock::time_point recoveryExpiresAt_{}, restoreBlockedUntil_{};
    unsigned congestionSamples_ = 0;
    bool background_ = false;
};
} // namespace ultraterimal::agent
