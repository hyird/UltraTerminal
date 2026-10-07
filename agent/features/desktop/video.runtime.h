#pragma once

#include <array>
#include <asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <ruvia/core/execution_handle.h>

#include "platform/windows/timer.transport.h"
#include "refinement.runtime.h"
#include "region_budget.types.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// One pending-pixel decision across source replacements. encodeAt is the
// earliest VIDEO start, not the optional deadline. Only output/baseline reset
// clears the anchor; retiring a source does not buy another allowance.
class DesktopRegionDecision {
  public:
    using Clock = std::chrono::steady_clock;
    std::chrono::microseconds remaining(Clock::time_point now, Clock::time_point encodeAt, std::chrono::microseconds allowance) {
        if (!deadline_) {
            deadline_ = (encodeAt > now ? encodeAt : now) + allowance;
        }
        return now < *deadline_ ? std::chrono::duration_cast<std::chrono::microseconds>(*deadline_ - now) : std::chrono::microseconds::zero();
    }
    bool pending(Clock::time_point now) const { return deadline_ && now < *deadline_; }
    std::optional<Clock::time_point> deadline() const { return deadline_; }
    void reset() { deadline_.reset(); }

  private:
    std::optional<Clock::time_point> deadline_;
};
namespace detail {
// Module-private gate: CPU preparation must start in this dispatch. Encoder
// capability does not make an already materialized source asynchronous ROI.
constexpr bool regionPreparationReady(bool fullCpuPixels, bool readbackReady, DesktopRegionDecision::Clock::time_point now, DesktopRegionDecision::Clock::time_point encodeAt) {
    return !fullCpuPixels || (readbackReady && now >= encodeAt);
}
} // namespace detail
// Immutable worker-local admission evidence. Durations use this worker's local
// epoch only; they are not timestamps for wire transport or cross-machine use.
struct DesktopAdmissionSnapshot {
    std::uint64_t id = 0, jobId = 0, revision = 0;
    std::uint32_t framesPerSecond = 0, bitrate = 0, workUnits = 0;
    std::chrono::microseconds allowance{}, window{}, remainingAtAdmission{};
    std::chrono::steady_clock::duration started{}, admittedAt{}, deadline{}, encodeAt{};
    std::optional<std::chrono::steady_clock::duration> decisionDeadline, sharedProbeBefore;
    // Diagnostic bitset: unknown prepare/send, fit, advantage, unavailable
    // normal (e.g. cooldown), and recent shared probe. Not a new policy gate.
    std::uint32_t probeReasons = 0;
    std::optional<std::chrono::microseconds> preparePrediction, sendPrediction;
    std::optional<std::uint64_t> messageBytes;
    std::chrono::microseconds videoEncodeEstimate{}, videoAlternative{}, preparationPrefix{};
    bool comparison = false; // True zero-send stage, not an unknown PNG product.
    std::uint64_t videoBytes = 0;
    DesktopPreparationSource source = DesktopPreparationSource::kCpu;
    DesktopRegionCost::Admission choice = DesktopRegionCost::Admission::kDenied;
    bool region = false, fitDenied = false, advantageDenied = false, unknownPrediction = false;
    std::uint64_t captureEpoch = 0, sourceSerial = 0, sourceGeneration = 0, composedBase = 0;
    std::uint32_t width = 0, height = 0, rotation = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
    bool operator==(const DesktopAdmissionSnapshot&) const = default;
};
// One owned association per produced batch. Admission can precede completion in
// another dispatch; never recover it from mutable last-optional diagnostics.
struct DesktopRefinementAdmission {
    DesktopAdmissionSnapshot admission;
    DesktopDetailWorkKey work;
    DesktopDetailBatchKey batch;
    std::uint64_t completedJobId = 0;
    bool operator==(const DesktopRefinementAdmission&) const = default;
};
// Worker-local routing/production evidence only, never protocol telemetry.
struct DesktopOptionalDiagnostics {
    std::uint64_t normal = 0, probe = 0, fitDenied = 0, advantageDenied = 0, unknownPrediction = 0;
    std::uint64_t cpuSource = 0, roiSource = 0;
    std::uint64_t actualPngBytes = 0, knownPngProducts = 0, unknownPngProducts = 0;
    std::uint64_t reliableReferences = 0, localPngReuses = 0;
    std::uint64_t materializeXorCursor = 0, materializeLegacyDetail = 0, materializeSoftware = 0, materializeGpuFallback = 0;
    std::optional<std::uint64_t> predictedMessageBytes, predictedPrepareMicroseconds, lastPngBytes;
};
struct DesktopPrimaryAdmission {
    DesktopAdmissionSnapshot admission;
    std::uint64_t generation = 0, frame = 0, base = 0, completedJobId = 0;
    bool operator==(const DesktopPrimaryAdmission&) const = default;
};
// Bounded worker-local stage evidence; queue bytes are not GPU execution time.
struct DesktopComparisonDiagnostics {
    std::uint64_t baselineCommits = 0, baselineFailures = 0, baselineCpuMicroseconds = 0, baselineGpuBytes = 0;
    std::uint64_t submissions = 0, finishes = 0, failures = 0, proofs = 0, unchanged = 0, rejectedProofs = 0;
    std::uint64_t lastPixels = 0, lastGpuBytes = 0, lastBaselineSerial = 0, lastBaselineFrame = 0;
    std::uint32_t lastObservationWorkUnits = 0;
    std::chrono::microseconds lastSpent{}, lastMinimumCpu{}, lastOverlap{};
    std::optional<DesktopAdmissionSnapshot> lastAdmission;
};
enum class DesktopReadbackCheckpointKind { kSubmitStart,
                                           kSubmitEnd,
                                           kCheckStart,
                                           kCheckEnd,
                                           kReady,
                                           kBeforeAcquire,
                                           kConsumeReady,
                                           kDecisionExpired,
                                           kRetireStart,
                                           kRetireEnd,
                                           kCostFinish,
                                           kPrepareStart,
                                           kPrepareEnd,
                                           kVideoFallback };
enum class DesktopVideoFallbackReason { kNone,
                                        kDecisionExpiredBeforeCheck,
                                        kDecisionWindowEnded,
                                        kSourceReplacement,
                                        kIdentityOrRevisionMismatch,
                                        kReadbackError,
                                        kBusy,
                                        kUnavailable,
                                        kNoReadyOrTimer,
                                        kPrepareRejected,
                                        kProofRejected,
                                        kOtherRetirement };
struct DesktopReadbackCheckpoint {
    DesktopReadbackCheckpointKind kind = DesktopReadbackCheckpointKind::kSubmitStart;
    std::uint64_t jobId = 0, revision = 0, captureEpoch = 0, sourceSerial = 0, generation = 0, composedBase = 0;
    std::chrono::steady_clock::duration at{};
    std::chrono::microseconds remaining{}, cpuMinimum{}, overlappingWork{};
    std::uint32_t observationWorkUnits = 8192;
    WindowsTimerResolutionStatus timer;
    std::optional<std::uint32_t> status;
    std::optional<std::int32_t> error;
    std::optional<std::chrono::microseconds> finishedPreparePrediction;
    bool atOrAfterDeadline = false, physicalNeedsDrain = false, readyOwned = false;
};
struct DesktopComparisonBaselineKey {
    std::uint64_t captureEpoch = 0, sourceSerial = 0, generation = 0, composedBase = 0;
    std::uint32_t width = 0, height = 0, rotation = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
    bool operator==(const DesktopComparisonBaselineKey&) const = default;
};
// Reason bits describe observed PNG rejection, not another policy gate.
// 1 revision, 2 deadline, 4 bytes, 8 missing patch, 16 atomic cold multi-tile,
// 32 interrupted preparation. Presence distinguishes not attempted/not finished.
struct DesktopPngAttemptDiagnostic {
    std::uint32_t width = 0, height = 0;
    bool encoded = false, patch = false;
    std::optional<std::uint64_t> bytes;
    DesktopStageDurations stages;
};
struct DesktopComparisonPngDiagnostic {
    DesktopAdmissionSnapshot admission; // Copied at CPU admission, never lastAdmission.
    std::optional<std::chrono::steady_clock::duration> cooldownUntil;
    bool cooldownActive = false, preparationAttempted = false;
    std::uint32_t attemptedUnits = 8192, count = 0, rejectionReasons = 0;
    std::array<DesktopPngAttemptDiagnostic, 32> attempts{};
    std::optional<std::chrono::microseconds> finishCost;
    std::optional<std::chrono::steady_clock::duration> finishedAt;
    bool finishSuccess = false, cacheCommitted = false, cacheAborted = false;
};
// One most-recent submitted work, not a ring and not global lastAdmission.
// Checkpoints with another work ID are never attached to this admission.
struct DesktopReadbackDiagnostic {
    DesktopAdmissionSnapshot admission;
    std::uint32_t purpose = 0; // 0 comparison, 1 ordinary REGION, 2 static, 3 dynamic.
    std::optional<DesktopDetailWorkKey> detailWork;
    std::optional<DesktopComparisonBaselineKey> comparisonBaseline;
    std::optional<DesktopComparisonPngDiagnostic> png;
    std::uint32_t submittedTiles = 0, submittedWorkUnits = 0;
    std::uint64_t submittedPixels = 0;
    DesktopVideoFallbackReason retirementReason = DesktopVideoFallbackReason::kNone;
    DesktopVideoFallbackReason videoFallbackReason = DesktopVideoFallbackReason::kNone;
    std::uint64_t fallbackJobId = 0, fallbackSourceSerial = 0;
    std::array<DesktopReadbackCheckpoint, 32> checkpoints{};
    std::uint32_t count = 0, dropped = 0;
};
// Process-local counters for diagnosing capture/pacing without recording pixels.
struct DesktopVideoDiagnostics {
    std::uint64_t captureCalls = 0;
    std::uint64_t pixelUpdates = 0;
    std::uint64_t cursorOnlyUpdates = 0;
    std::uint64_t captureTimeouts = 0;
    std::uint64_t pendingBeforeDeadlineCalls = 0;
    std::uint64_t backpressuredCalls = 0;
    // Worker-produced VIDEO updates are not owner-loop publications. Rejected
    // updates stay counted; videoEncodeCalls also includes failed/aborted work.
    std::uint64_t encodedFrames = 0;
    std::uint64_t returnedSnapshots = 0, videoEncodeCalls = 0;
    std::uint64_t publishedVideoFrames = 0, rejectedPrivateVideoFrames = 0, rejectedPrivateJobs = 0;
    std::uint64_t lastJobRevision = 0, ownerRevision = 0;
    std::uint64_t lastAcquireEpoch = 0, lastAcquireSerial = 0, lastAcquirePresentTicks = 0;
    bool lastJobAcquired = false, lastJobConsumedReady = false;
    std::uint64_t pixelCaptureGaps = 0;
    std::uint64_t pixelCaptureGapsGe45ms = 0;
    std::uint64_t unknownDamageUpdates = 0;
    std::uint64_t regionCandidates = 0;
    std::uint64_t regionBudgetFailures = 0;
    std::uint64_t regionTimeBudgetFailures = 0;
    std::uint64_t filteredPixels = 0;
    std::uint64_t unchangedPixelUpdates = 0;
    std::uint64_t sparseReadbackPixels = 0;
    std::uint64_t refinementBatches = 0, refinementTiles = 0;
    std::uint64_t dynamicRefinementBatches = 0;
    std::uint64_t roiSubmissions = 0, roiPending = 0, roiBusy = 0, roiRetired = 0, roiFailures = 0;
    std::uint64_t optionalCostDenials = 0;
    std::uint64_t regionDecisionWaits = 0, regionDecisionExpirations = 0;
    WindowsTimerResolutionStatus timerResolution;
    DesktopOptionalDiagnostics optional;
    std::optional<DesktopAdmissionSnapshot> lastAdmission;
    // Owner-loop accepted batch only; rejected private results cannot replace it.
    std::optional<DesktopRefinementAdmission> latestPublishedRefinement;
    std::optional<DesktopPrimaryAdmission> latestPublishedRegion;
    DesktopComparisonDiagnostics comparison;
    std::optional<DesktopReadbackDiagnostic> lastReadback;
};
struct DesktopVideoUpdate {
    std::optional<wire::DesktopFlowSample> flowSample; // Input-channel diagnostics.
    std::optional<wire::VideoConfig> configuration;
    std::optional<wire::CursorUpdate> cursor;
    std::optional<wire::VideoRefinement> refinement;
    std::optional<DesktopRefinementAdmission> refinementAdmission; // Local sidecar, never serialized.
    std::optional<DesktopPrimaryAdmission> regionAdmission;
    std::optional<wire::DesktopRegionUpdate> region;
    // Region updates advance the composed frame ID without encoding video.
    wire::VideoFrame frame;
    std::uint64_t generation = 0;
    std::uint64_t frameId = 0;
    std::uint64_t baseFrameId = 0;
    std::uint64_t refinementFrameId = 0;
    std::uint64_t changedPixels = 0;
    // Local-only QPC tick of the most recent pixel snapshot (not serialized).
    std::uint64_t sourcePresentTicks = 0;
    std::uint32_t workerDispatchMicroseconds = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
};
// One owner on the desktop process event loop. Capture and encoding run on a
// dedicated bounded worker. Await next() before calling it again or destroying
// the runtime. stop() interrupts pacing; an in-flight codec operation is joined.
class DesktopVideoRuntime {
  public:
    DesktopVideoRuntime(ruvia::execution_handle loop, std::uint32_t displayIndex, std::uint32_t serverOptimizationContract = 0, bool serverContinuousFrames = false);
    ~DesktopVideoRuntime();
    DesktopVideoRuntime(const DesktopVideoRuntime&) = delete;
    DesktopVideoRuntime& operator=(const DesktopVideoRuntime&) = delete;
    asio::awaitable<std::optional<DesktopVideoUpdate>> next(bool refresh = false);
    // Call between next() operations; display changes run on the capture worker.
    asio::awaitable<bool> resize(std::uint32_t width, std::uint32_t height);
    void stop();       // Owning event loop only.
    void invalidate(); // Discard an in-flight old frame and rebuild the source.
    void feedback(const wire::QualityFeedback& feedback);
    void capabilities(const wire::DesktopVideoCapabilities& capabilities);
    // Pending joint permission only; immutable Server support is not changed.
    // Activation requires Browser caps2 and a new Config/generation barrier.
    void setOptimizationContract(std::uint32_t contract);
    // Separate from mergeable QualityFeedback; exact APPLIED/SUPERSEDED tuple.
    void refinementAck(const wire::Envelope& acknowledgement);
    void observeRoundTrip(std::chrono::microseconds elapsed);
    // Owning event loop only; most recent completed capture-worker snapshot.
    DesktopVideoDiagnostics diagnostics() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
