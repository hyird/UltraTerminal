#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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
// Worker-local durations only; overlapping stages are intentionally not summed.
// Presence distinguishes an unexecuted stage from a measured zero duration.
struct DesktopStageDurations {
    std::optional<std::uint32_t> workerQueue, acquireWait, sourceAge, pacingCreditWait;
    std::optional<std::uint32_t> gpuSyncWait, readbackReady, prepareCpu, pngEncode, videoEncode;
    std::uint32_t coalescedCaptures = 0;
    static std::uint32_t microseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), 0, 60000000));
    }
};

enum class DesktopPreparationSource { kCpu,
                                      kRoi };
// Shared by region/detail and both preparation histories on one worker. A
// source/shape change cannot manufacture another unknown/slow probe slot.
struct DesktopOptionalProbe {
    std::optional<std::chrono::steady_clock::time_point> last;
};
// Preparation fits the original COMPUTE deadline; preparation + actual-size
// network prediction competes with video. Tokens/byte/lease limits are separate.
class DesktopRegionCost {
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::microseconds;
    enum class Admission { kDenied,
                           kNormal,
                           kProbe };
    explicit DesktopRegionCost(DesktopOptionalProbe* sharedProbe = nullptr) : sharedProbe_(sharedProbe) {}
    void begin(Clock::time_point now, Duration allowance, DesktopPreparationSource source = DesktopPreparationSource::kCpu, std::uint32_t workUnits = 1) {
        started_ = now;
        deadline_ = now + std::clamp(allowance, Duration::zero(), Duration(60000000));
        source_ = source;
        workUnits_ = std::max(1u, workUnits);
        observationWorkUnits_ = workUnits_;
        admitted_ = false;
    }
    std::optional<Duration> preparePrediction(std::uint32_t units = 0) const {
        const auto& h = history();
        if (!h.count) {
            return std::nullopt; // In particular ROI may not borrow CPU history.
        }
        double density = h.ewma;
        for (std::size_t i = 0; i < h.count; ++i) {
            density = std::max(density, h.samples[i]);
        }
        return Duration(static_cast<Duration::rep>(std::clamp(density * (units ? units : workUnits_), 250.0, 60000000.0)));
    }
    Duration prediction(Duration estimatedSend = Duration::zero()) const {
        return preparePrediction().value_or(Duration(250)) + std::max(estimatedSend, Duration::zero());
    }
    // Metadata-only prefix sizing, no admission/probe/deadline mutation.
    // preparationPrefix is sunk comparison work: include it in advantage,
    // not remaining compute time or this product's preparation history.
    bool normalCandidateFits(Clock::time_point now, Duration videoCost, std::optional<Duration> send, std::uint32_t units, Duration preparationPrefix = Duration::zero()) const {
        const auto prepare = preparePrediction(units);
        return prepare && send && videoCost > Duration::zero() && now < deadline_ && now >= history().cooldownUntil &&
               *prepare <= std::chrono::duration_cast<Duration>(deadline_ - now) && hasAdvantage(*prepare + std::max(*send, Duration::zero()) + std::max(preparationPrefix, Duration::zero()), videoCost);
    }
    void predictionWorkUnits(std::uint32_t units) { workUnits_ = std::max(1u, units); }
    bool admit(Clock::time_point now, Duration videoCost, std::optional<Duration> estimatedSend = Duration::zero(), Duration preparationPrefix = Duration::zero()) {
        admitted_ = false;
        admission_ = Admission::kDenied;
        const auto prepare = preparePrediction();
        unknown_ = !prepare || !estimatedSend;
        fitDenied_ = now >= deadline_ || (prepare && *prepare > std::chrono::duration_cast<Duration>(deadline_ - now));
        const auto alternative = prepare.value_or(Duration(250)) + std::max(estimatedSend.value_or(Duration::zero()), Duration::zero()) + std::max(preparationPrefix, Duration::zero());
        auto& h = history();
        advantageDenied_ = estimatedSend && !hasAdvantage(alternative, videoCost);
        if (now >= deadline_ || videoCost <= Duration::zero()) {
            return false;
        }
        if (!unknown_ && !fitDenied_ && !advantageDenied_ && now >= h.cooldownUntil) {
            h.selected = admitted_ = true;
            admission_ = Admission::kNormal;
            return true;
        }
        h.selected = false;
        auto& last = sharedProbe_ ? sharedProbe_->last : probe_.last;
        if (last && now - *last < std::chrono::seconds(1)) {
            return false;
        }
        last = now;
        // Runtime bounds a cold/slow probe to one uncancellable tile call.
        // The original 250us boundary and absolute deadline remain unchanged.
        admitted_ = deadline_ - now >= Duration(250);
        admission_ = admitted_ ? Admission::kProbe : Admission::kDenied;
        return admitted_;
    }
    void observe(Duration elapsed, bool success, Clock::time_point now) {
        admitted_ = false;
        elapsed = std::clamp(elapsed, Duration::zero(), Duration(60000000));
        auto& h = history();
        const auto density = static_cast<double>(elapsed.count()) / observationWorkUnits_;
        h.samples[h.next] = density;
        h.next = (h.next + 1) % h.samples.size();
        h.count = std::min(h.count + 1, h.samples.size());
        h.ewma = h.count == 1 ? density : h.ewma * 0.75 + density * 0.25;
        if (!success || now >= deadline_) {
            h.selected = false;
            h.cooldownUntil = now + std::chrono::seconds(1);
            (sharedProbe_ ? sharedProbe_->last : probe_.last) = now;
        }
    }
    void finish(Clock::time_point now, bool success, Duration overlappingWork = Duration::zero(), Duration minimumWork = Duration::zero()) {
        // Subtract the one disjoint pacing/video overlap only once. Real
        // submit/Map/copy/PNG CPU is a lower bound even for retired/failed work.
        observe(std::max({Duration::zero(), minimumWork, std::chrono::duration_cast<Duration>(now - started_) - std::max(overlappingWork, Duration::zero())}), success, now);
    }
    // An early stop must not divide sunk cost by tiles that were never tried.
    // This changes only observation scale, never admission or its deadline.
    void observationWorkUnits(std::uint32_t units) { observationWorkUnits_ = std::max(1u, units); }
    bool within(Clock::time_point now) const { return now < deadline_; }
    bool admitted() const { return admitted_; }
    Admission admission() const { return admission_; }
    bool fitDenied() const { return fitDenied_; }
    bool advantageDenied() const { return advantageDenied_; }
    bool unknownPrediction() const { return unknown_; }
    DesktopPreparationSource source() const { return source_; }
    // Read-only diagnostic access; does not select/reset either cost history.
    Clock::time_point cooldownUntil() const { return history().cooldownUntil; }

  private:
    struct History {
        std::array<double, 8> samples{};
        std::size_t next = 0, count = 0;
        double ewma = 0;
        Clock::time_point cooldownUntil{};
        bool selected = false;
    };
    bool hasAdvantage(Duration alternative, Duration videoCost) const {
        return history().selected ? alternative.count() * 4 <= videoCost.count() * 5 : alternative.count() * 5 <= videoCost.count() * 4;
    }
    History& history() { return histories_[source_ == DesktopPreparationSource::kRoi]; }
    const History& history() const { return histories_[source_ == DesktopPreparationSource::kRoi]; }
    std::array<History, 2> histories_{};
    DesktopOptionalProbe probe_;
    DesktopOptionalProbe* sharedProbe_;
    Clock::time_point started_{}, deadline_{};
    DesktopPreparationSource source_ = DesktopPreparationSource::kCpu;
    std::uint32_t workUnits_ = 1, observationWorkUnits_ = 1;
    Admission admission_ = Admission::kDenied;
    bool admitted_ = false, fitDenied_ = false, advantageDenied_ = false, unknown_ = false;
};
} // namespace ultraterimal::agent
