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
// Frame ACKs release credit; RTT probes and sustained ACK delivery estimate
// path capacity. Static-desktop idle time is excluded from delivery samples.
class DesktopFrameFlow {
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
    static constexpr std::size_t kMaximumInFlightFrames = 128;
    static constexpr std::uint32_t kMinimumFramesPerSecond = 5, kMaximumFramesPerSecond = 60;
    static constexpr std::uint32_t kMinimumVideoBitrate = 64000, kMaximumVideoBitrate = 100000000;
    static constexpr std::uint32_t kLegacyMaximumInFlightFrames = 16;
    Rate rate() const { return rate_; }
    bool background() const { return background_; }
    bool detailAllowed() const { return quality_ != Quality::kSmooth; }
    std::pair<std::uint32_t, std::uint32_t> geometry() const { return {width_, height_}; }
    bool setGeometry(std::uint32_t width, std::uint32_t height) {
        if (width < 16 || height < 16 || width > 4096 || height > 4096 || (width & 1) || (height & 1) || static_cast<std::uint64_t>(width) * height > 4096ull * 2160) {
            return false;
        }
        width_ = width;
        height_ = height;
        scenePixels_.reset();
        sceneBytes_ = 0;
        updateFrameRate();
        return true;
    }
    bool setQuality(Quality quality) {
        if (quality == quality_) {
            return false;
        }
        quality_ = quality;
        rate_.sharp = quality == Quality::kSharp;
        // A preference change must not erase an observed narrow-path limit.
        rate_.bitrate = std::min(rate_.bitrate, preferredBitrate());
        updateFrameRate();
        return true;
    }
    void reset(std::uint64_t generation) {
        generation_ = generation;
        pending_.clear();
        pendingBytes_ = 0;
        acknowledged_ = lastSent_ = 0;
        lastAcknowledgedAt_ = {};
        baseline_.reset();
        deliveryBytesPerSecond_ = 0;
        clientFrameLimit_ = kLegacyMaximumInFlightFrames;
        receiverFrameRate_ = kMaximumFramesPerSecond;
        decodeMicroseconds_ = 0;
        scenePixels_.reset();
        sceneBytes_ = 0;
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
    void observeEncode(std::chrono::microseconds elapsed) {
        if (elapsed.count() > 0) {
            encodeMicroseconds_ = encodeMicroseconds_ ? (encodeMicroseconds_ * 7 + static_cast<std::uint64_t>(elapsed.count())) / 8 : static_cast<std::uint64_t>(elapsed.count());
            updateFrameRate();
        }
    }
    void observeScene(std::size_t bytes, std::uint64_t changedPixels, bool keyFrame) {
        if (keyFrame) {
            return; // Startup/refresh bursts do not describe steady frame cost.
        }
        const auto pixels = std::min(changedPixels, static_cast<std::uint64_t>(width_) * height_);
        scenePixels_ = scenePixels_ ? (*scenePixels_ * 3 + pixels) / 4 : pixels;
        sceneBytes_ = sceneBytes_ ? (sceneBytes_ * 3 + bytes) / 4 : bytes;
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
        return static_cast<std::size_t>(std::clamp<std::uint64_t>(bytes, 16384, 4 * 1024 * 1024));
    }
    bool canEncode(Clock::time_point now) const {
        return !background_ && (pending_.empty() || (pending_.size() < frameWindow() && pendingBytes_ < byteWindow() && now - pending_.front().sent <= pathDelay() + std::chrono::milliseconds(150)));
    }
    std::size_t pendingBytes() const { return pendingBytes_; }
    std::size_t pendingFrames() const { return pending_.size(); }
    void sent(std::uint64_t generation, std::uint64_t frame, Clock::time_point now, std::size_t bytes) {
        if (generation != generation_) {
            if (!generation || generation < generation_) {
                throw std::logic_error("Retired desktop generation sent");
            }
            reset(generation);
        }
        if (!frame || frame <= lastSent_ || pending_.size() >= kMaximumInFlightFrames || bytes > std::numeric_limits<std::size_t>::max() - pendingBytes_) {
            throw std::logic_error("Desktop frame exceeded receiver-credit safety bounds");
        }
        pending_.push_back({frame, now, bytes});
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
        const auto transit = elapsed - std::min(elapsed, std::chrono::microseconds(decodeMicroseconds));
        if (!baseline_ || now >= baselineExpiresAt_) {
            baseline_ = transit;
            baselineExpiresAt_ = now + std::chrono::seconds(30);
        } else {
            baseline_ = std::min(*baseline_, transit);
        }
        std::size_t delivered = 0;
        for (auto current = pending_.begin(); current != std::next(found); ++current) {
            delivered += current->bytes;
        }
        const auto interval = std::chrono::duration_cast<std::chrono::microseconds>(now - lastAcknowledgedAt_);
        if (lastAcknowledgedAt_ != Clock::time_point{} && found->sent <= lastAcknowledgedAt_ && interval >= std::chrono::milliseconds(5)) {
            const auto sample = std::min<std::uint64_t>(100000000, static_cast<std::uint64_t>(delivered) * 1000000 / static_cast<std::uint64_t>(interval.count()));
            deliveryBytesPerSecond_ = deliveryBytesPerSecond_ ? (deliveryBytesPerSecond_ * 3 + sample) / 4 : sample;
        }
        pendingBytes_ -= delivered;
        pending_.erase(pending_.begin(), std::next(found));
        acknowledged_ = frame;
        lastAcknowledgedAt_ = now;
        setReceiver(background, advertisedMaximumPendingFrames);
        if (background_) {
            return true;
        }
        decodeMicroseconds_ = decodeMicroseconds_ ? (decodeMicroseconds_ * 7 + decodeMicroseconds) / 8 : decodeMicroseconds;
        const bool networkPressure = transit > pathDelay() + std::chrono::milliseconds(50);
        const bool decoderPressure = queueDepth > 2 || decodeMicroseconds_ > 25000;
        const bool congested = networkPressure || decoderPressure;
        congestionSamples_ = congested ? std::min(3u, congestionSamples_ + 1) : 0;
        if (congested) {
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
                if (deliveryBytesPerSecond_) {
                    target = std::min(target, deliveryBytesPerSecond_ * 8 * 9 / 10);
                }
                rate_.bitrate = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(target, kMinimumVideoBitrate, preferredBitrate()));
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
            const auto bitrateLimit = recoveryRate_ ? std::min(preferredBitrate(), recoveryRate_->bitrate) : preferredBitrate();
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
    struct Pending {
        std::uint64_t frame;
        Clock::time_point sent;
        std::size_t bytes;
    };
    std::chrono::microseconds pathDelay() const {
        return std::clamp(roundTrip_.value_or(baseline_.value_or(std::chrono::microseconds(100000))), std::chrono::microseconds(1000), std::chrono::microseconds(2000000));
    }
    std::uint32_t preferredBitrate() const {
        return quality_ == Quality::kSharp ? 24000000u : quality_ == Quality::kSmooth ? 8000000u
                                                                                      : 16000000u;
    }
    void setReceiver(bool background, std::uint32_t capacity) {
        clientFrameLimit_ = capacity ? capacity : kLegacyMaximumInFlightFrames;
        if (background != background_) {
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
        const auto bitsPerFrame = std::max<std::uint64_t>(clarityBits, sceneBytes_ * 8 * 11 / 10);
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
    Quality quality_ = Quality::kSharp;
    std::uint32_t width_ = 1920, height_ = 1080, clientFrameLimit_ = kLegacyMaximumInFlightFrames;
    std::uint32_t receiverFrameRate_ = kMaximumFramesPerSecond;
    std::uint64_t generation_ = 0, acknowledged_ = 0, lastSent_ = 0;
    std::uint64_t deliveryBytesPerSecond_ = 0, encodeMicroseconds_ = 0, decodeMicroseconds_ = 0;
    std::optional<std::uint64_t> scenePixels_;
    std::uint64_t sceneBytes_ = 0;
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
