#include "video.runtime.h"

#include <algorithm>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/blocking_pool.h>

#include "damage.protocol.h"
#include "frame_cadence.runtime.h"
#include "frame_flow.runtime.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/cursor.transport.h"
#include "platform/windows/deadline_timer.transport.h"
#include "recovery.runtime.h"
#include "refinement.transport.h"
#include "tile_cache.transport.h"
#include "video.transport.h"

namespace ultraterimal::agent {
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
namespace detail {
// Fixture variant only; no public declaration or production hook.
void beforeDesktopJobReturnFixture(const std::optional<DesktopVideoUpdate>&, const DesktopVideoDiagnostics&);
} // namespace detail
#endif
namespace {
using VideoClock = std::chrono::steady_clock;
void addDuration(std::optional<std::uint32_t>& total, std::optional<std::uint32_t> sample) {
    if (sample) {
        total = std::min(60000000u, total.value_or(0) + *sample);
    }
}
void writeStages(wire::DesktopStageTiming& target, const DesktopStageDurations& stages) {
    if (stages.workerQueue) {
        target.set_worker_queue_microseconds(*stages.workerQueue);
    }
    if (stages.acquireWait) {
        target.set_acquire_wait_microseconds(*stages.acquireWait);
    }
    if (stages.sourceAge) {
        target.set_source_age_microseconds(*stages.sourceAge);
    }
    if (stages.pacingCreditWait) {
        target.set_pacing_credit_wait_microseconds(*stages.pacingCreditWait);
    }
    if (stages.gpuSyncWait) {
        target.set_gpu_sync_wait_microseconds(*stages.gpuSyncWait);
    }
    if (stages.readbackReady) {
        target.set_readback_ready_microseconds(*stages.readbackReady);
    }
    if (stages.prepareCpu) {
        target.set_prepare_cpu_microseconds(*stages.prepareCpu);
    }
    if (stages.pngEncode) {
        target.set_png_encode_microseconds(*stages.pngEncode);
    }
    if (stages.videoEncode) {
        target.set_video_encode_microseconds(*stages.videoEncode);
    }
    target.set_coalesced_captures(stages.coalescedCaptures);
}
std::optional<std::uint32_t> sourceAge(std::uint64_t ticks) {
    LARGE_INTEGER now{}, frequency{};
    if (!ticks || !QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || static_cast<std::uint64_t>(now.QuadPart) < ticks) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::min(60000000.0, static_cast<double>(static_cast<std::uint64_t>(now.QuadPart) - ticks) * 1000000.0 / static_cast<double>(frequency.QuadPart)));
}
// Accessed exclusively by the single encoding worker, including construction
// and destruction of DXGI/D3D and codec objects.
struct VideoWorker {
    // Declared first, destroyed last: the request outlives capture/codec/ROI
    // objects, including an in-flight worker job and its cancelled readback.
    WindowsTimerResolution timerResolution;
    DesktopRegionDecision regionDecision;
    std::unique_ptr<DesktopCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::optional<CapturedFrame> retained;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::uint64_t generation = 0, contentFrameId = 0, deliveryFrameId = 0, lastPresentTicks = 0;
    std::uint64_t lastEncodedTicks = 0, refinedFrame = 0;
    std::chrono::steady_clock::time_point lastEncodedAt{};
    std::chrono::steady_clock::time_point lastContentAt{};
    std::chrono::steady_clock::time_point previousPixelCaptureAt{};
    std::uint32_t width = 0, height = 0;
    std::uint64_t configuredCaptureEpoch = 0;
    std::int32_t configuredLeft = 0, configuredTop = 0;
    bool configurationPending = true;
    bool pixelsPending = false;
    bool readbackReady = false;
    bool capturedOnGpu = false;
    std::chrono::steady_clock::time_point encodeAt{};
    std::chrono::steady_clock::time_point compareAt{};
    DesktopCursor cursor;
    CaptureRecovery recovery;
    DesktopVideoDiagnostics diagnostics;
    std::uint64_t jobId = 0, admissionId = 0;
    DesktopAdmissionSnapshot regionAdmission, detailAdmission, comparisonAdmission;
    std::optional<DesktopReadbackTag> comparisonBaseline;
    DesktopDamage damage;
    DesktopTileCache tiles;
    DesktopRegionBudget regionBudget;
    std::uint32_t damageWidth = 0, damageHeight = 0;
    std::vector<wire::VideoCodec> requestedCodecs{wire::AVC};
    // The owner loop only touches this atomic cancellation serial, never DXGI,
    // encoders, planner or pixel/cache state. WIC is checked between tiles.
    std::atomic<std::uint64_t> requestedRevision{0};
    bool optimizationV2 = false, continuousFrames = false, fullPixelsValid = false, staticCruise = false;
    std::uint64_t retirementSerial = 0;
    std::uint32_t coalescedCaptures = 0;
    DesktopRefinementPlan refinementPlan;
    DesktopDetailTokens refinementTokens;
    DesktopOptionalProbe optionalProbe;
    DesktopRegionCost regionCost{&optionalProbe}, detailCost{&optionalProbe}, comparisonCost{&optionalProbe};
    std::chrono::microseconds comparisonMinimumWork{}, comparisonOverlap{};
    std::chrono::microseconds regionOverlap{}, detailOverlap{}, regionMinimumWork{}, detailMinimumWork{};
    std::chrono::microseconds videoEstimate{4000};
    std::size_t lastVideoBytes = 16000, dynamicCursor = 0;
    DesktopStageDurations stages;
    std::optional<std::uint32_t> pendingCpuMap, pendingCpuCopy;
    bool cpuMeasuredThisRound = false;
    std::chrono::microseconds optionalEncoderTime{}, earlyOptionalCompute{};
    bool encodeWindowStarted = false;
    enum class ReadbackPurpose { kComparison,
                                 kRegion,
                                 kStatic,
                                 kDynamic };
    struct ReadbackWork {
        ReadbackPurpose purpose;
        DesktopReadbackTag tag;
        std::vector<DesktopDetailTile> areas;
        std::optional<DesktopDetailWorkKey> workKey;
        std::optional<DesktopReadbackResult> ready;
        std::uint64_t revision;
        DesktopStageDurations stages;
        DesktopAdmissionSnapshot admission;
        std::optional<DesktopReadbackTag> baseline;
        bool proofApplied = false;
        std::chrono::microseconds comparisonPrefix{};
    };
    std::optional<ReadbackWork> readback;
    bool readbackNeedsDrain = false, readbackChecked = false, comparisonRetired = false;
    std::uint32_t readbackMapObserved = 0;
    std::uint64_t physicalTraceId = 0;
    std::uint32_t regionObservedUnits = 8192, detailObservedUnits = 8192, comparisonObservedUnits = 8192;

    std::uint32_t& purposeObservedUnits(ReadbackPurpose purpose) {
        return purpose == ReadbackPurpose::kComparison ? comparisonObservedUnits : purpose == ReadbackPurpose::kRegion ? regionObservedUnits
                                                                                                                       : detailObservedUnits;
    }
    void traceReadback(std::uint64_t id, DesktopReadbackCheckpointKind kind, VideoClock::time_point at,
                       std::optional<std::uint32_t> status = {}, std::optional<std::int32_t> error = {}) {
        auto& trace = diagnostics.lastReadback;
        if (!trace || trace->admission.id != id) {
            return; // No event may borrow a newer or unrelated admission.
        }
        const bool terminal = kind == DesktopReadbackCheckpointKind::kDecisionExpired || kind == DesktopReadbackCheckpointKind::kRetireStart ||
                              kind == DesktopReadbackCheckpointKind::kRetireEnd || kind == DesktopReadbackCheckpointKind::kCostFinish ||
                              kind == DesktopReadbackCheckpointKind::kVideoFallback || kind == DesktopReadbackCheckpointKind::kReady ||
                              kind == DesktopReadbackCheckpointKind::kConsumeReady || kind == DesktopReadbackCheckpointKind::kPrepareEnd ||
                              (kind == DesktopReadbackCheckpointKind::kCheckEnd && status && *status != static_cast<std::uint32_t>(DesktopReadbackStatus::kPending));
        // Reserve terminal evidence without a ring/overwrite or larger buffer.
        // Repeated Pending checkpoints may be dropped, never silently counted
        // as absent retirement/finish/Retired drain in the bounded sidecar.
        if (trace->count == trace->checkpoints.size() || (trace->count >= trace->checkpoints.size() - 8 && !terminal)) {
            ++trace->dropped;
            return;
        }
        auto& point = trace->checkpoints[trace->count++];
        point.kind = kind;
        point.jobId = jobId;
        point.revision = requestedRevision.load();
        if (retained) {
            point.captureEpoch = retained->captureEpoch;
            point.sourceSerial = retained->sourceSerial;
        }
        point.generation = generation;
        point.composedBase = contentFrameId;
        point.at = at - epoch;
        const auto deadline = trace->purpose <= 1 ? trace->admission.decisionDeadline.value_or(trace->admission.deadline) : trace->admission.deadline;
        point.atOrAfterDeadline = point.at >= deadline;
        point.remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - point.at); // Signed, never extends D.
        const auto purpose = static_cast<ReadbackPurpose>(trace->purpose);
        point.cpuMinimum = purposeMinimum(purpose);
        point.overlappingWork = purpose == ReadbackPurpose::kComparison ? comparisonOverlap : optionalEncoderTime + (purpose == ReadbackPurpose::kRegion ? regionOverlap : detailOverlap);
        point.observationWorkUnits = purposeObservedUnits(purpose);
        point.timer = timerResolution.status(); // Existing status only, no OS request/query.
        point.status = status;
        point.error = error;
        if (kind == DesktopReadbackCheckpointKind::kCostFinish) {
            point.finishedPreparePrediction = purposeCost(purpose).preparePrediction();
        }
        point.physicalNeedsDrain = readbackNeedsDrain;
        point.readyOwned = readback && readback->admission.id == id && readback->ready.has_value();
    }
    void traceRetirement(DesktopVideoFallbackReason reason, VideoClock::time_point at) {
        if (readback && diagnostics.lastReadback && diagnostics.lastReadback->admission.id == readback->admission.id) {
            diagnostics.lastReadback->retirementReason = reason;
            traceReadback(readback->admission.id, DesktopReadbackCheckpointKind::kRetireStart, at);
        }
    }

    static bool primaryReadback(ReadbackPurpose purpose) { return purpose == ReadbackPurpose::kRegion || purpose == ReadbackPurpose::kComparison; }
    DesktopRegionCost& purposeCost(ReadbackPurpose purpose) {
        return purpose == ReadbackPurpose::kComparison ? comparisonCost : purpose == ReadbackPurpose::kRegion ? regionCost
                                                                                                              : detailCost;
    }
    std::chrono::microseconds& purposeMinimum(ReadbackPurpose purpose) {
        return purpose == ReadbackPurpose::kComparison ? comparisonMinimumWork : purpose == ReadbackPurpose::kRegion ? regionMinimumWork
                                                                                                                     : detailMinimumWork;
    }
    DesktopRegionCost& readbackCost() { return purposeCost(readback->purpose); }
    void clearComparisonBaseline() {
        comparisonBaseline.reset();
        if (capture) {
            capture->clearTextureBaseline();
        }
    }
    void commitComparisonBaseline() {
        if (!optimizationV2 || !encoder->gpu()) {
            clearComparisonBaseline();
            return;
        }
        const auto started = VideoClock::now();
        std::optional<DesktopTextureBaselineResult> result;
        try {
            auto queued = capture->rememberTextureBaseline(retained->readbackTag(generation, contentFrameId));
            if (queued) {
                result = *queued;
            }
        } catch (...) {
            // This auxiliary copy must not erase an already encoded primary.
            // Without a new exact key, the next source uses VIDEO/ordinary ROI.
        }
        const auto elapsed = DesktopStageDurations::microseconds(started, VideoClock::now());
        addDuration(stages.prepareCpu, elapsed); // Allocation/compiler/copy included once.
        diagnostics.comparison.baselineCpuMicroseconds += elapsed;
        diagnostics.comparison.baselineGpuBytes += result ? result->copiedBytes : 0;
        if (result && result->status == DesktopTextureBaselineStatus::kQueued) {
            comparisonBaseline = retained->readbackTag(generation, contentFrameId);
            ++diagnostics.comparison.baselineCommits;
        } else {
            ++diagnostics.comparison.baselineFailures;
            clearComparisonBaseline(); // NEVER retain the old composed-base key.
        }
    }
    std::chrono::microseconds finishComparison(bool success) {
        if (!comparisonCost.admitted()) {
            return readback ? readback->comparisonPrefix : std::chrono::microseconds::zero();
        }
        const auto now = VideoClock::now();
        const auto wall = std::max(std::chrono::microseconds::zero(), std::chrono::duration_cast<std::chrono::microseconds>(now - epoch - comparisonAdmission.started));
        const auto overlap = std::min(wall, comparisonOverlap); // Pacer interval only; no inherited codec overlap.
        const auto spent = std::max(comparisonMinimumWork, wall - overlap);
        comparisonCost.finish(now, success, overlap, comparisonMinimumWork);
        traceReadback(comparisonAdmission.id, DesktopReadbackCheckpointKind::kCostFinish, now);
        auto& d = diagnostics.comparison;
        ++d.finishes;
        d.failures += !success;
        d.lastSpent = spent;
        d.lastMinimumCpu = comparisonMinimumWork;
        d.lastOverlap = overlap;
        return spent;
    }
    std::chrono::microseconds sendEstimate(std::size_t bytes, DesktopFrameFlow::Rate rate) const {
        return std::chrono::microseconds(static_cast<std::int64_t>(std::min<std::uint64_t>(60000000, static_cast<std::uint64_t>(bytes) * 8000000 / rate.bitrate)));
    }
    std::chrono::microseconds videoAlternative(DesktopFrameFlow::Rate rate) const {
        return videoEstimate + sendEstimate(lastVideoBytes, rate);
    }
    std::chrono::microseconds optionalAllowance(DesktopFrameFlow::Rate rate) const {
        return std::chrono::microseconds(std::clamp(500000u / rate.framesPerSecond, 4000u, 12000u));
    }
    std::chrono::microseconds primaryAllowance(DesktopFrameFlow::Rate rate) const {
        // One bounded frame period covers asynchronous GPU completion. Half a
        // period made ordinary driver latency trigger a full second of VIDEO
        // cooldown. The decision fixes this allowance at admission, never on a
        // later wake or source change; auxiliary work keeps its smaller budget.
        return std::chrono::microseconds(std::clamp(1000000u / rate.framesPerSecond, 4000u, 16000u));
    }
    std::optional<std::size_t> predictedMessageBytes(std::span<const DesktopDetailTile> areas, std::size_t copyBytes = 0) const {
        std::size_t bytes = 528 + copyBytes; // Existing body/stage/envelope reservation.
        for (const auto& area : areas) {
            const auto png = tiles.predictedPngBytes(area.width, area.height);
            if (!png) {
                return std::nullopt;
            }
            // Conservative full PNG until prepare validates a reliable reference.
            // v2 local reuse ALWAYS sends that PNG, not an empty reference.
            bytes += *png + 64;
        }
        return bytes;
    }
    bool admitOptional(DesktopRegionCost& cost, VideoClock::time_point now, std::chrono::microseconds allowance,
                       std::span<const DesktopDetailTile> areas, DesktopFrameFlow::Rate rate, std::size_t copyBytes = 0,
                       std::size_t* tileLimit = nullptr, std::size_t byteBudget = 0,
                       std::optional<DesktopPreparationSource> preparationSource = {}, bool comparison = false,
                       std::chrono::microseconds preparationPrefix = {}, DesktopComparisonPngDiagnostic* pngEvidence = nullptr) {
        const auto source = preparationSource.value_or(fullPixelsValid || areas.empty() ? DesktopPreparationSource::kCpu : DesktopPreparationSource::kRoi);
        std::uint32_t units = 8192;
        for (const auto& area : areas) {
            units += area.width * area.height + 8192;
        }
        cost.begin(now, allowance, source, units);
        cost.observationWorkUnits(8192); // Unattempted tiles cannot dilute sunk cost.
        (&cost == &comparisonCost ? comparisonObservedUnits : &cost == &regionCost ? regionObservedUnits
                                                                                   : detailObservedUnits) = 8192;
        if (tileLimit && !areas.empty()) {
            // Detail is independently publishable per tile. Size a prefix from
            // known cost/bytes, within the SAME deadline and original <=8 cap.
            // No probe, pixel scan, plan advance or codec call occurs here.
            *tileLimit = 1; // Unknown/slow work retains the one-tile cold probe.
            for (std::size_t count = areas.size(); count; --count) {
                const auto prefix = areas.first(count);
                const auto bytes = predictedMessageBytes(prefix, copyBytes);
                std::uint32_t prefixUnits = 8192;
                for (const auto& area : prefix) {
                    prefixUnits += area.width * area.height + 8192;
                }
                if (bytes && *bytes <= byteBudget && cost.normalCandidateFits(VideoClock::now(), videoAlternative(rate), sendEstimate(*bytes, rate), prefixUnits, preparationPrefix)) {
                    *tileLimit = count;
                    break;
                }
            }
            areas = areas.first(*tileLimit);
            units = 8192;
            for (const auto& area : areas) {
                units += area.width * area.height + 8192;
            }
            cost.predictionWorkUnits(units); // Does not re-begin or extend the deadline.
        }
        const auto bytes = comparison ? std::optional<std::size_t>(0) : predictedMessageBytes(areas, copyBytes);
        auto& d = diagnostics.optional;
        d.predictedMessageBytes = bytes;
        const auto prepare = cost.preparePrediction();
        d.predictedPrepareMicroseconds = prepare ? std::optional<std::uint64_t>(prepare->count()) : std::nullopt;
        d.cpuSource += source == DesktopPreparationSource::kCpu;
        d.roiSource += source == DesktopPreparationSource::kRoi;
        const auto send = bytes ? std::optional(sendEstimate(*bytes, rate)) : std::nullopt;
        const auto alternative = videoAlternative(rate);
        const auto admittedAt = VideoClock::now();
        const auto sharedProbeBefore = optionalProbe.last;
        const bool admitted = cost.admit(admittedAt, alternative, send, preparationPrefix);
        const auto window = std::clamp(allowance, std::chrono::microseconds::zero(), std::chrono::microseconds(60000000));
        const auto tag = retained->readbackTag(generation, contentFrameId);
        const DesktopAdmissionSnapshot snapshot{
            .id = ++admissionId,
            .jobId = jobId,
            .revision = diagnostics.lastJobRevision,
            .framesPerSecond = rate.framesPerSecond,
            .bitrate = rate.bitrate,
            .workUnits = units,
            .allowance = &cost == &detailCost ? optionalAllowance(rate) : primaryAllowance(rate),
            .window = window,
            .remainingAtAdmission = std::chrono::duration_cast<std::chrono::microseconds>(now + window - admittedAt),
            .started = now - epoch,
            .admittedAt = admittedAt - epoch,
            .deadline = now + window - epoch,
            .encodeAt = encodeAt - epoch,
            .decisionDeadline = regionDecision.deadline() ? std::optional(*regionDecision.deadline() - epoch) : std::nullopt,
            .sharedProbeBefore = sharedProbeBefore ? std::optional(*sharedProbeBefore - epoch) : std::nullopt,
            .probeReasons = (!prepare ? 1u : 0u) | (!send ? 2u : 0u) | (cost.fitDenied() ? 4u : 0u) | (cost.advantageDenied() ? 8u : 0u) |
                            (prepare && send && !cost.fitDenied() && !cost.advantageDenied() && cost.admission() != DesktopRegionCost::Admission::kNormal ? 16u : 0u) |
                            (sharedProbeBefore && admittedAt - *sharedProbeBefore < std::chrono::seconds(1) ? 32u : 0u),
            .preparePrediction = prepare,
            .sendPrediction = send,
            .messageBytes = bytes,
            .videoEncodeEstimate = videoEstimate,
            .videoAlternative = alternative,
            .preparationPrefix = preparationPrefix,
            .comparison = comparison,
            .videoBytes = lastVideoBytes,
            .source = source,
            .choice = cost.admission(),
            .region = &cost == &regionCost,
            .fitDenied = cost.fitDenied(),
            .advantageDenied = cost.advantageDenied(),
            .unknownPrediction = cost.unknownPrediction(),
            .captureEpoch = tag.captureEpoch,
            .sourceSerial = tag.sourceSerial,
            .sourceGeneration = tag.generation,
            .composedBase = tag.composedBase,
            .width = tag.width,
            .height = tag.height,
            .rotation = tag.rotation,
            .desktopLeft = tag.desktopLeft,
            .desktopTop = tag.desktopTop};
        if (pngEvidence) {
            pngEvidence->admission = snapshot;
            const auto cooldown = cost.cooldownUntil();
            pngEvidence->cooldownUntil = cooldown != VideoClock::time_point{} ? std::optional(cooldown - epoch) : std::nullopt;
            pngEvidence->cooldownActive = admittedAt < cost.cooldownUntil();
        }
        diagnostics.lastAdmission = snapshot;
        if (admitted) {
            (&cost == &comparisonCost ? comparisonAdmission : &cost == &regionCost ? regionAdmission
                                                                                   : detailAdmission) = snapshot;
        }
        if (comparison) {
            diagnostics.comparison.lastAdmission = snapshot;
        }
        d.normal += cost.admission() == DesktopRegionCost::Admission::kNormal;
        d.probe += cost.admission() == DesktopRegionCost::Admission::kProbe;
        d.fitDenied += cost.fitDenied();
        d.advantageDenied += cost.advantageDenied();
        d.unknownPrediction += cost.unknownPrediction();
        if (!admitted) {
            ++diagnostics.optionalCostDenials;
        }
        return admitted;
    }
    DesktopComparisonPngDiagnostic* comparisonPng() {
        if (!diagnostics.lastReadback || diagnostics.lastReadback->purpose != static_cast<std::uint32_t>(ReadbackPurpose::kComparison) || !diagnostics.lastReadback->png ||
            diagnostics.lastReadback->png->admission.id != regionAdmission.id) {
            return nullptr;
        }
        return &*diagnostics.lastReadback->png;
    }
    void finishRegion(VideoClock::time_point now, bool success, std::chrono::microseconds overlap, std::chrono::microseconds minimum) {
        if (auto* png = comparisonPng()) {
            const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(now - epoch - png->admission.started);
            png->finishCost = std::max({std::chrono::microseconds::zero(), minimum, wall - std::max(overlap, std::chrono::microseconds::zero())});
            png->finishedAt = now - epoch;
            png->finishSuccess = success;
        }
        regionCost.finish(now, success, overlap, minimum); // Original failed-tail learning, exactly once.
    }
    void observeTile(const DesktopTileObservation& tile) {
        auto& d = diagnostics.optional;
        d.lastPngBytes = tile.pngBytes;
        if (tile.pngBytes) {
            ++d.knownPngProducts;
            d.actualPngBytes += *tile.pngBytes;
        } else if (tile.encoded) {
            ++d.unknownPngProducts;
        }
        d.reliableReferences += tile.reliableReference;
        d.localPngReuses += tile.localReuse;
    }
    void takeReadbackStages() {
        if (!readback) {
            return;
        }
        addDuration(stages.gpuSyncWait, readback->stages.gpuSyncWait);
        addDuration(stages.prepareCpu, readback->stages.prepareCpu);
        addDuration(stages.readbackReady, readback->stages.readbackReady);
        readback->stages = {};
    }
    void retireReadback(DesktopVideoFallbackReason reason = DesktopVideoFallbackReason::kOtherRetirement) {
        const auto started = VideoClock::now();
        const auto traceId = readback ? readback->admission.id : physicalTraceId;
        traceRetirement(reason, started);
        if (readback) {
            takeReadbackStages();
            if (readback->purpose == ReadbackPurpose::kComparison) {
                if (readbackNeedsDrain) {
                    comparisonRetired = true; // Retain charge until the same physical slot truly drains.
                } else {
                    finishComparison(false);
                }
            } else {
                const auto finished = VideoClock::now();
                readbackCost().finish(finished, false, optionalEncoderTime + (readback->purpose == ReadbackPurpose::kRegion ? regionOverlap : detailOverlap), purposeMinimum(readback->purpose));
                traceReadback(traceId, DesktopReadbackCheckpointKind::kCostFinish, finished);
            }
            readback.reset();
            ++diagnostics.roiRetired;
        }
        if (capture && readbackNeedsDrain) {
            capture->retireTextureRegions();
        }
        if (comparisonRetired) {
            comparisonMinimumWork += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - started);
        }
        traceReadback(traceId, DesktopReadbackCheckpointKind::kRetireEnd, VideoClock::now());
    }
    void retireWork(DesktopVideoFallbackReason reason = DesktopVideoFallbackReason::kOtherRetirement) {
        retireReadback(reason);
        if (regionCost.admitted()) {
            finishRegion(VideoClock::now(), false, optionalEncoderTime + regionOverlap, regionMinimumWork);
        }
        if (detailCost.admitted()) {
            detailCost.finish(VideoClock::now(), false, optionalEncoderTime + detailOverlap, detailMinimumWork);
        }
        if (!comparisonRetired) {
            finishComparison(false);
        }
        refinementPlan.retire();
        staticCruise = false;
        ++retirementSerial;
    }
    bool current(std::uint64_t revision) const { return requestedRevision.load() == revision; }
    void checkReadback() {
        if (!capture || !readbackNeedsDrain || readbackChecked || !retained || (readback && readback->ready)) {
            return;
        }
        readbackChecked = true;
        const auto expected = readback ? readback->tag : retained->readbackTag(generation, contentFrameId);
        const auto started = VideoClock::now();
        const auto traceId = readback ? readback->admission.id : physicalTraceId;
        traceReadback(traceId, DesktopReadbackCheckpointKind::kCheckStart, started);
        auto result = [&]() {
            try {
                return capture->tryCompleteTextureRegions(expected);
            } catch (...) {
                const auto elapsed = std::chrono::microseconds(DesktopStageDurations::microseconds(started, VideoClock::now()));
                if (!encodeWindowStarted) {
                    earlyOptionalCompute += elapsed;
                }
                if (readback) {
                    purposeMinimum(readback->purpose) += elapsed;
                } else if (comparisonRetired) {
                    comparisonMinimumWork += elapsed;
                }
                traceReadback(traceId, DesktopReadbackCheckpointKind::kCheckEnd, VideoClock::now());
                throw; // Retirement observes failed CPU before subtracting overlap.
            }
        }();
        const auto checkedAt = VideoClock::now();
        traceReadback(traceId, DesktopReadbackCheckpointKind::kCheckEnd, checkedAt,
                      result ? std::optional(static_cast<std::uint32_t>(result->status)) : std::nullopt,
                      result ? std::nullopt : std::optional(static_cast<std::int32_t>(result.error())));
        if (result && result->status == DesktopReadbackStatus::kReady) {
            traceReadback(traceId, DesktopReadbackCheckpointKind::kReady, checkedAt);
        }
        const auto elapsed = DesktopStageDurations::microseconds(started, VideoClock::now());
        if (!encodeWindowStarted) {
            earlyOptionalCompute += std::chrono::microseconds(elapsed);
        }
        if (!readback && comparisonRetired) {
            comparisonMinimumWork += std::chrono::microseconds(elapsed);
        }
        if (readback) {
            purposeMinimum(readback->purpose) += std::chrono::microseconds(elapsed);
            if (readback->purpose == ReadbackPurpose::kComparison && result) {
                diagnostics.comparison.lastGpuBytes = result->timing.gpuBytes.value_or(0);
            }
        }
        if (!result) {
            ++diagnostics.roiFailures;
            retireReadback(DesktopVideoFallbackReason::kReadbackError);
            return;
        }
        auto& measured = readback ? readback->stages : stages;
        std::optional<std::uint32_t> mapDelta;
        if (result->timing.mapMicroseconds) {
            // G reports cumulative Map duration for the physical slot. Never
            // add its prefix again on every Pending check or after retirement.
            mapDelta = *result->timing.mapMicroseconds - std::min(readbackMapObserved, *result->timing.mapMicroseconds);
            readbackMapObserved = *result->timing.mapMicroseconds;
        }
        addDuration(measured.gpuSyncWait, mapDelta);
        // Dispatch is already inside the measured submit CPU interval. Here
        // only row-copy and non-Map completion bookkeeping are CPU preparation.
        addDuration(measured.prepareCpu, result->timing.rowCopyMicroseconds);
        const auto known = static_cast<std::uint64_t>(mapDelta.value_or(0)) + result->timing.rowCopyMicroseconds.value_or(0);
        addDuration(measured.prepareCpu, static_cast<std::uint32_t>(elapsed - std::min<std::uint64_t>(elapsed, known)));
        addDuration(measured.readbackReady, result->timing.readbackReadyMicroseconds);
        if (result->status == DesktopReadbackStatus::kPending) {
            ++diagnostics.roiPending;
            return;
        }
        readbackNeedsDrain = false;
        if (comparisonRetired) {
            finishComparison(false);
            comparisonRetired = false;
        }
        if (readback && result->status == DesktopReadbackStatus::kReady && result->result && result->result->tag == readback->tag) {
            readback->ready = std::move(result->result);
            diagnostics.sparseReadbackPixels += readback->ready->pixels;
        } else {
            retireReadback(DesktopVideoFallbackReason::kIdentityOrRevisionMismatch);
        }
    }
    bool submitReadback(ReadbackPurpose purpose, std::vector<DesktopDetailTile> areas, std::optional<DesktopDetailWorkKey> workKey,
                        std::uint64_t revision) {
        const auto started = VideoClock::now();
        std::vector<DesktopReadbackRegion> regions;
        for (const auto& area : areas) {
            regions.push_back({area.x, area.y, area.width, area.height});
        }
        const auto tag = retained->readbackTag(generation, contentFrameId);
        auto& minimum = purposeMinimum(purpose);
        diagnostics.lastReadback.emplace();
        auto& trace = *diagnostics.lastReadback;
        trace.admission = purpose == ReadbackPurpose::kComparison ? comparisonAdmission : purpose == ReadbackPurpose::kRegion ? regionAdmission
                                                                                                                              : detailAdmission;
        trace.purpose = static_cast<std::uint32_t>(purpose);
        trace.detailWork = workKey;
        if (purpose == ReadbackPurpose::kComparison && comparisonBaseline) {
            const auto& b = *comparisonBaseline;
            trace.comparisonBaseline = DesktopComparisonBaselineKey{b.captureEpoch, b.sourceSerial, b.generation, b.composedBase, b.width, b.height, b.rotation, b.desktopLeft, b.desktopTop};
        }
        const auto traceId = trace.admission.id;
        traceReadback(traceId, DesktopReadbackCheckpointKind::kSubmitStart, started);
        auto submitted = [&]() {
            try {
                return purpose == ReadbackPurpose::kComparison ? capture->submitTextureComparison(tag, *comparisonBaseline, regions) : capture->submitTextureRegions(tag, regions);
            } catch (...) {
                minimum += std::chrono::microseconds(DesktopStageDurations::microseconds(started, VideoClock::now()));
                traceReadback(traceId, DesktopReadbackCheckpointKind::kSubmitEnd, VideoClock::now());
                throw;
            }
        }();
        const auto submittedAt = VideoClock::now();
        traceReadback(traceId, DesktopReadbackCheckpointKind::kSubmitEnd, submittedAt,
                      submitted ? std::optional(static_cast<std::uint32_t>(*submitted)) : std::nullopt,
                      submitted ? std::nullopt : std::optional(static_cast<std::int32_t>(submitted.error())));
        const auto submitCpu = DesktopStageDurations::microseconds(started, VideoClock::now());
        minimum += std::chrono::microseconds(submitCpu);
        if (!submitted || *submitted != DesktopReadbackSubmit::kSubmitted) {
            addDuration(stages.prepareCpu, submitCpu);
            trace.retirementReason = !submitted ? DesktopVideoFallbackReason::kReadbackError : *submitted == DesktopReadbackSubmit::kBusy ? DesktopVideoFallbackReason::kBusy
                                                                                                                                          : DesktopVideoFallbackReason::kUnavailable;
            if (purpose == ReadbackPurpose::kComparison) {
                finishComparison(false);
            } else {
                const auto finished = VideoClock::now();
                purposeCost(purpose).finish(finished, false, purpose == ReadbackPurpose::kRegion ? regionOverlap : detailOverlap, minimum);
                traceReadback(traceId, DesktopReadbackCheckpointKind::kCostFinish, finished);
            }
            if (submitted && *submitted == DesktopReadbackSubmit::kBusy) {
                ++diagnostics.roiBusy;
                readbackNeedsDrain = true;
            } else {
                ++diagnostics.roiFailures;
            }
            return false;
        }
        readback = ReadbackWork{purpose, tag, std::move(areas), workKey, std::nullopt, revision, {}, purpose == ReadbackPurpose::kComparison ? comparisonAdmission : purpose == ReadbackPurpose::kRegion ? regionAdmission
                                                                                                                                                                                                         : detailAdmission};
        if (purpose == ReadbackPurpose::kComparison) {
            readback->baseline = comparisonBaseline;
            std::uint32_t units = 8192;
            std::uint64_t pixels = 0;
            for (const auto& area : readback->areas) {
                units += area.width * area.height + 8192;
                pixels += static_cast<std::uint64_t>(area.width) * area.height;
            }
            comparisonCost.observationWorkUnits(units); // All dispatched pixels, not WIC attempts.
            comparisonObservedUnits = units;
            diagnostics.comparison.lastObservationWorkUnits = units;
            diagnostics.comparison.lastPixels = pixels;
            ++diagnostics.comparison.submissions;
        }
        physicalTraceId = traceId;
        trace.submittedTiles = static_cast<std::uint32_t>(readback->areas.size());
        trace.submittedWorkUnits = 8192;
        for (const auto& area : readback->areas) {
            trace.submittedPixels += static_cast<std::uint64_t>(area.width) * area.height;
            trace.submittedWorkUnits += area.width * area.height + 8192;
        }
        readback->stages.prepareCpu = submitCpu;
        readbackMapObserved = 0;
        readbackNeedsDrain = true;
        ++diagnostics.roiSubmissions;
        if (purpose != ReadbackPurpose::kDynamic) {
            checkReadback(); // Dynamic's sole probe follows normal video encoding.
        }
        return true;
    }
    DesktopDetailPixels detailPixels(const DesktopDetailTile& area, const DesktopReadbackResult* packed) const {
        if (!packed) {
            const auto offset = (static_cast<std::size_t>(area.y) * retained->width + area.x) * 4;
            if (!fullPixelsValid || offset >= retained->bgra.size()) {
                return {};
            }
            return {std::span(retained->bgra).subspan(offset), area.width, area.height, static_cast<std::size_t>(retained->width) * 4};
        }
        std::size_t offset = 0;
        for (const auto& region : packed->regions) {
            const auto bytes = static_cast<std::size_t>(region.width) * region.height * 4;
            if (offset > packed->bgra.size() || bytes > packed->bgra.size() - offset) {
                return {};
            }
            if (region.x == area.x && region.y == area.y && region.width == area.width && region.height == area.height) {
                return {std::span(packed->bgra).subspan(offset, bytes), area.width, area.height, static_cast<std::size_t>(area.width) * 4};
            }
            offset += bytes;
        }
        return {};
    }
    std::optional<wire::VideoRefinement> prepareDetail(const std::vector<DesktopDetailTile>& areas, const DesktopReadbackResult* packed,
                                                       DesktopFrameFlow::Rate rate, std::size_t byteBudget, std::uint64_t revision, wire::RefinementKind kind,
                                                       const DesktopAdmissionSnapshot& admission, std::optional<DesktopRefinementAdmission>& association) {
        const auto prepareStarted = VideoClock::now();
        wire::VideoRefinement detail;
        detail.set_kind(kind);
        std::uint32_t pixels = 0;
        std::size_t reserved = 512;
        bool complete = true;
        std::uint32_t observedUnits = 8192;
        for (const auto& area : areas) {
            if (!current(revision) || !detailCost.within(VideoClock::now()) || reserved + 64 >= byteBudget) {
                break;
            }
            observedUnits += area.width * area.height + 8192;
            detailCost.observationWorkUnits(observedUnits);
            detailObservedUnits = observedUnits;
            const auto predicted = predictedMessageBytes(std::span(&area, 1));
            auto attempt = prepareVideoDetail(detailPixels(area, packed), area, tiles, refinementTokens, detailCost, byteBudget - reserved,
                                              videoAlternative(rate), predicted ? std::optional(sendEstimate(*predicted, rate)) : std::nullopt, false);
            observeTile(attempt.tile);
            addDuration(stages.prepareCpu, attempt.stages.prepareCpu);
            addDuration(stages.pngEncode, attempt.stages.pngEncode);
            if (attempt.patch) {
                reserved += attempt.patch->ByteSizeLong() + 64;
                pixels += area.width * area.height;
                *detail.add_patches() = std::move(*attempt.patch);
            }
            // No stronger promise than returning from one synchronous WIC call.
            if (attempt.failed || !current(revision) || !detailCost.within(VideoClock::now())) {
                complete = false;
                break;
            }
        }
        const auto finished = VideoClock::now();
        writeStages(*detail.mutable_stage_timing(), stages);
        // Include timing and the worst uint64 batch field before committing the
        // cumulative/message budget; the final envelope reserve remains bounded.
        const auto batchBytes = detail.ByteSizeLong() + 528;
        const bool valid = complete && current(revision) && detailCost.within(finished) && detail.patches_size() && batchBytes <= byteBudget;
        detailMinimumWork += std::chrono::duration_cast<std::chrono::microseconds>(finished - prepareStarted);
        detailCost.finish(finished, valid, optionalEncoderTime + detailOverlap, detailMinimumWork);
        traceReadback(admission.id, DesktopReadbackCheckpointKind::kCostFinish, finished);
        if (!valid) {
            return std::nullopt;
        }
        const auto work = refinementPlan.workKey();
        if (!work) {
            return std::nullopt;
        }
        const auto key = refinementPlan.publish(finished, *work, static_cast<std::uint32_t>(detail.patches_size()), batchBytes, pixels);
        if (!key) {
            return std::nullopt;
        }
        detail.set_batch_id(key->batch);
        association = DesktopRefinementAdmission{admission, *work, *key, jobId};
        ++diagnostics.refinementBatches;
        diagnostics.refinementTiles += detail.patches_size();
        diagnostics.dynamicRefinementBatches += kind == wire::DYNAMIC_DETAIL;
        return detail;
    }

    bool applyComparison() {
        const auto started = VideoClock::now();
        const auto& work = *readback;
        const auto& packed = *work.ready;
        bool valid = current(work.revision) && comparisonBaseline && work.baseline == comparisonBaseline &&
                     regionDecision.pending(started) && comparisonCost.within(started) && packed.tag == work.tag &&
                     packed.comparison && packed.comparison->baselineTag == *work.baseline &&
                     packed.regions.size() == work.areas.size() && packed.comparison->changed.size() == work.areas.size();
        std::uint64_t pixels = 0;
        std::vector<DesktopDamageComparisonTile> proof;
        if (valid) {
            for (std::size_t i = 0; i < work.areas.size(); ++i) {
                const auto& area = work.areas[i];
                const auto& region = packed.regions[i];
                valid &= region.x == area.x && region.y == area.y && region.width == area.width && region.height == area.height;
                pixels += static_cast<std::uint64_t>(area.width) * area.height;
                proof.push_back({area.x, area.y, area.width, area.height, packed.comparison->changed[i]});
            }
            valid &= pixels <= 262144 && packed.pixels == pixels && packed.bgra.size() == pixels * 4;
        }
        auto filtered = damage; // Transactional proof; deadline failure must not alter the original damage.
        valid = valid && regionDecision.pending(VideoClock::now()) && filtered.filterComparedTiles(proof) && regionDecision.pending(VideoClock::now());
        comparisonMinimumWork += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - started);
        if (!valid) {
            ++diagnostics.comparison.rejectedProofs;
            retireReadback(DesktopVideoFallbackReason::kProofRejected);
            return false;
        }
        damage = std::move(filtered);
        ++diagnostics.comparison.proofs;
        diagnostics.comparison.lastBaselineSerial = work.baseline->sourceSerial;
        diagnostics.comparison.lastBaselineFrame = work.baseline->composedBase;
        readback->comparisonPrefix = finishComparison(true);
        readback->proofApplied = true;
        return true;
    }
    void consumeUnchanged() {
        ++diagnostics.unchangedPixelUpdates;
        regionDecision.reset();
        pixelsPending = false;
        lastEncodedTicks = lastPresentTicks;
        damage.commit();
        coalescedCaptures = 0;
        pendingCpuMap.reset();
        pendingCpuCopy.reset();
        refinementPlan.resumeCanvas(generation, contentFrameId, width, height);
    }
    std::optional<wire::DesktopRegionUpdate> prepareRegion(DesktopFrameFlow::Rate rate, std::size_t byteBudget, std::uint64_t revision,
                                                           const DesktopReadbackResult* packed, bool& unchanged) {
        const auto prepareStarted = VideoClock::now();
        auto* png = comparisonPng();
        if (png) {
            png->preparationAttempted = true;
        }
        struct PreparationExit {
            DesktopComparisonPngDiagnostic* png;
            bool complete = false;
            ~PreparationExit() {
                if (png && !complete) {
                    png->rejectionReasons |= 32u;
                    png->cacheAborted = true;
                }
            }
        } preparationExit{png};
        const auto traceId = readback && readback->purpose == ReadbackPurpose::kRegion ? readback->admission.id : 0;
        traceReadback(traceId, DesktopReadbackCheckpointKind::kPrepareStart, prepareStarted);
        if (!packed && fullPixelsValid) {
            const auto previous = damage.changedPixels();
            damage.filterPixels(retained->bgra); // Only known dirty/move tiles.
            diagnostics.filteredPixels += previous - std::min(previous, damage.changedPixels());
            unchanged = damage.unchanged();
            addDuration(stages.prepareCpu, DesktopStageDurations::microseconds(prepareStarted, VideoClock::now()));
            if (unchanged) {
                regionMinimumWork += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - prepareStarted);
                finishRegion(VideoClock::now(), true, regionOverlap, regionMinimumWork);
                preparationExit.complete = true;
                return std::nullopt;
            }
        }
        wire::DesktopRegionUpdate region;
        region.set_target_frames_per_second(rate.framesPerSecond);
        region.set_target_bitrate(rate.bitrate);
        for (const auto& copy : damage.copies()) {
            *region.add_copies() = copy;
        }
        tiles.begin();
        bool fits = true;
        std::uint32_t observedUnits = 8192;
        const auto patches = damage.patches();
        for (const auto& area : patches) {
            if (!current(revision) || !regionCost.within(VideoClock::now()) || region.ByteSizeLong() + 512 >= byteBudget) {
                if (png) {
                    png->rejectionReasons |= (!current(revision) ? 1u : 0u) | (!regionCost.within(VideoClock::now()) ? 2u : 0u) | (region.ByteSizeLong() + 512 >= byteBudget ? 4u : 0u);
                }
                fits = false;
                break;
            }
            observedUnits += area.width() * area.height() + 8192;
            regionCost.observationWorkUnits(observedUnits);
            regionObservedUnits = observedUnits;
            DesktopStageDurations tileStages;
            DesktopTileObservation observation;
            DesktopPngAttemptDiagnostic* attempt = nullptr;
            if (png) {
                png->attemptedUnits = observedUnits;
                if (png->count < png->attempts.size()) {
                    attempt = &png->attempts[png->count++];
                    attempt->width = area.width();
                    attempt->height = area.height();
                }
            }
            std::optional<wire::PatchRect> patch;
            if (packed) {
                const DesktopDetailTile tile{area.x(), area.y(), area.width(), area.height()};
                const auto view = detailPixels(tile, packed);
                wire::Rectangle local;
                local.set_width(tile.width);
                local.set_height(tile.height);
                if (view.valid()) {
                    patch = tiles.prepare(view.bgra, tile.width, tile.height, local, tileCacheEnabled, byteBudget - region.ByteSizeLong() - 512, nullptr, &tileStages, &observation);
                    if (patch) {
                        *patch->mutable_destination() = area;
                    }
                }
            } else {
                patch = tiles.prepare(retained->bgra, retained->width, retained->height, area, tileCacheEnabled, byteBudget - region.ByteSizeLong() - 512, nullptr, &tileStages, &observation);
            }
            if (attempt) {
                *attempt = DesktopPngAttemptDiagnostic{area.width(), area.height(), observation.encoded, patch.has_value(), observation.pngBytes, tileStages};
            }
            observeTile(observation);
            addDuration(stages.prepareCpu, tileStages.prepareCpu);
            addDuration(stages.pngEncode, tileStages.pngEncode);
            if (!patch) {
                if (png) {
                    png->rejectionReasons |= 8u;
                }
                fits = false;
                break;
            }
            *region.add_patches() = std::move(*patch);
            if (!current(revision) || (regionCost.admission() == DesktopRegionCost::Admission::kProbe && patches.size() > 1)) {
                // One cold WIC call; never publish a partial reliable batch.
                if (png) {
                    png->rejectionReasons |= (!current(revision) ? 1u : 0u) | (regionCost.admission() == DesktopRegionCost::Admission::kProbe && patches.size() > 1 ? 16u : 0u);
                }
                fits = false;
                break;
            }
        }
        writeStages(*region.mutable_stage_timing(), stages);
        fits &= regionCost.within(VideoClock::now()) && current(revision) && region.ByteSizeLong() + 528 <= byteBudget;
        regionMinimumWork += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - prepareStarted);
        const auto finishedAt = VideoClock::now();
        if (png) {
            png->rejectionReasons |= (!current(revision) ? 1u : 0u) | (!regionCost.within(finishedAt) ? 2u : 0u) | (region.ByteSizeLong() + 528 > byteBudget ? 4u : 0u);
        }
        finishRegion(finishedAt, fits, regionOverlap, regionMinimumWork);
        traceReadback(traceId, DesktopReadbackCheckpointKind::kCostFinish, finishedAt);
        traceReadback(traceId, DesktopReadbackCheckpointKind::kPrepareEnd, VideoClock::now());
        if (!fits) {
            if (diagnostics.lastReadback && diagnostics.lastReadback->admission.id == traceId) {
                diagnostics.lastReadback->retirementReason = DesktopVideoFallbackReason::kPrepareRejected;
            }
            tiles.begin();
            if (png) {
                png->cacheAborted = true;
            }
            preparationExit.complete = true;
            ++diagnostics.regionBudgetFailures;
            return std::nullopt;
        }
        // Reliable slots are committed only with a complete ordered update.
        tiles.commit();
        if (png) {
            png->cacheCommitted = true;
        }
        preparationExit.complete = true;
        return region;
    }
    bool tileCacheEnabled = false;
    void suspend() {
        if (readback || readbackNeedsDrain) {
            discard(); // Destroy pending async resource owners before timer end.
        } else {
            retireWork();
            regionDecision.reset();
        }
        timerResolution.setRequested(false);
        diagnostics.timerResolution = timerResolution.status();
    }
    void discard() {
        clearComparisonBaseline();
        regionDecision.reset();
        retireWork();
        retained.reset();
        encoder.reset();
        capture.reset();
        finishComparison(false); // Resource destruction terminates retired work too.
        comparisonRetired = false;
        cursor = DesktopCursor{};
        configurationPending = true;
        pixelsPending = false;
        lastPresentTicks = 0;
        lastEncodedTicks = refinedFrame = 0;
        lastEncodedAt = {};
        lastContentAt = {};
        previousPixelCaptureAt = {};
        damage.reset(0, 0);
        damageWidth = damageHeight = 0;
        tiles.clear();
        regionBudget = DesktopRegionBudget{};
        compareAt = {};
        fullPixelsValid = false;
        coalescedCaptures = 0;
        pendingCpuMap.reset();
        pendingCpuCopy.reset();
        readbackNeedsDrain = false;
    }
    enum class MaterializeReason { kCursor,
                                   kLegacyDetail,
                                   kSoftware,
                                   kGpuFallback };
    bool materialize(std::span<const wire::Rectangle> regions = {}, MaterializeReason reason = MaterializeReason::kSoftware) {
        try {
            diagnostics.optional.materializeXorCursor += reason == MaterializeReason::kCursor;
            if (regions.empty()) {
                auto& d = diagnostics.optional;
                d.materializeLegacyDetail += reason == MaterializeReason::kLegacyDetail;
                d.materializeSoftware += reason == MaterializeReason::kSoftware;
                d.materializeGpuFallback += reason == MaterializeReason::kGpuFallback;
                CaptureTiming timing;
                capture->readTexture(*retained, &timing);
                fullPixelsValid = true;
                cpuMeasuredThisRound = true;
                pendingCpuMap = timing.stages.mapMicroseconds;
                pendingCpuCopy = timing.stages.rowCopyMicroseconds;
                addDuration(stages.gpuSyncWait, timing.stages.mapMicroseconds);
                addDuration(stages.prepareCpu, timing.stages.rowCopyMicroseconds);
            } else {
                CaptureTiming timing;
                diagnostics.sparseReadbackPixels += capture->readTextureRegions(*retained, regions, &timing);
                addDuration(stages.gpuSyncWait, timing.stages.mapMicroseconds);
                addDuration(stages.prepareCpu, timing.stages.rowCopyMicroseconds);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    bool comparePixels() {
        try {
            if (encoder->gpu()) {
                if (const auto changed = capture->changedTextureTiles()) {
                    damage.filterTiles(*changed);
                }
            } else {
                damage.filterPixels(retained->bgra);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    bool rememberPixels() {
        try {
            if (encoder->gpu() || !fullPixelsValid) {
                damage.forgetPixels();
                if (encoder->gpu() && !optimizationV2) {
                    capture->rememberTexture();
                }
            } else {
                damage.rememberPixels(retained->bgra);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    std::optional<DesktopVideoUpdate> next(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                                           std::uint32_t flowWidth, std::uint32_t flowHeight,
                                           std::optional<std::pair<std::uint32_t, std::uint32_t>>& geometryChanged, bool allowEncode, bool allowRefinement, const wire::DesktopVideoCapabilities& capabilities,
                                           bool requestedV2, bool requestedContinuous, std::size_t detailBudget, std::size_t regionByteBudget, std::optional<std::pair<DesktopDetailBatchKey, DesktopDetailAck>> acknowledgement,
                                           std::uint64_t revision, DesktopStageDurations initialStages, std::optional<DesktopDetailBatchKey> rejectedPublication) try {
        const auto cadenceStarted = VideoClock::now();
        ++jobId;
        if (!requestedV2 && timerResolution.status().requested) {
            suspend();
        }
        timerResolution.setRequested(requestedV2);
        diagnostics.timerResolution = timerResolution.status();
        diagnostics.lastJobRevision = revision;
        diagnostics.lastJobAcquired = false;
        diagnostics.lastJobConsumedReady = false;
        stages = initialStages;
        optionalEncoderTime = {};
        earlyOptionalCompute = {};
        encodeWindowStarted = false;
        cpuMeasuredThisRound = false;
        readbackChecked = false;
        tileCacheEnabled = capabilities.tile_cache();
        if (capabilities.region_updates() || allowRefinement) {
            const auto preparationStarted = VideoClock::now();
            if (tiles.initializePng()) {
                addDuration(stages.prepareCpu, DesktopStageDurations::microseconds(preparationStarted, VideoClock::now()));
            }
        }
        if (rejectedPublication) {
            refinementPlan.rejectPublication(*rejectedPublication);
        }
        if (acknowledgement) {
            refinementPlan.acknowledge(VideoClock::now(), acknowledgement->first, acknowledgement->second);
        }
        refinementPlan.poll(VideoClock::now());
        refinementTokens.configure(VideoClock::now(), rate.bitrate / 8);
        if (reset) {
            discard();
        }
        std::vector<wire::VideoCodec> codecs{wire::AVC};
        for (const auto codec : capabilities.codecs()) {
            if (codec != wire::AVC) {
                codecs.push_back(static_cast<wire::VideoCodec>(codec));
            }
        }
        const bool codecChanged = codecs != requestedCodecs;
        const bool contractChanged = requestedV2 != optimizationV2 || requestedContinuous != continuousFrames;
        refresh = refresh || codecChanged || contractChanged;
        if (refresh) {
            retireWork();
        }
        if (!recovery.ready(std::chrono::steady_clock::now())) {
            return std::nullopt;
        }
        std::optional<CapturedFrame> pixels;
        try {
            if (!capture) {
                capture = std::make_unique<DesktopCapture>(display);
            }
            // DXGI wakes as soon as a desktop or cursor update is available.
            // The bounded wait also lets cancellation and rate changes take effect.
            if (readback && (readback->tag != retained->readbackTag(generation, contentFrameId) || !current(readback->revision))) {
                retireReadback(DesktopVideoFallbackReason::kIdentityOrRevisionMismatch);
            }
            if (readback && primaryReadback(readback->purpose)) {
                const auto decisionCheckedAt = VideoClock::now();
                if (!regionDecision.pending(decisionCheckedAt)) {
                    traceReadback(readback->admission.id, DesktopReadbackCheckpointKind::kDecisionExpired, decisionCheckedAt);
                    ++diagnostics.regionDecisionExpirations;
                    retireReadback(DesktopVideoFallbackReason::kDecisionExpiredBeforeCheck); // Original D, never another window.
                }
            }
            if (!readback || readback->purpose != ReadbackPurpose::kDynamic) {
                checkReadback(); // Observe/drain the old source before any overwrite.
            }
            // A valid Ready belongs to this retained source and its original
            // decision window. Consume it below BEFORE acquiring a new serial.
            // Pending does not prevent acquisition or extend the anchored D.
            const bool consumeReady = readback && readback->ready && readbackCost().within(VideoClock::now()) &&
                                      (!primaryReadback(readback->purpose) || regionDecision.pending(VideoClock::now())) &&
                                      allowEncode && !refresh && !configurationPending &&
                                      retained->width == flowWidth && retained->height == flowHeight;
            const auto now = std::chrono::steady_clock::now();
            const bool gpu = encoder && encoder->gpu();
            capturedOnGpu = gpu;
            const bool readPixels = !gpu && allowEncode && (!encoder || configurationPending || refresh || now >= encodeAt);
            const bool readyToEncode = retained && readPixels && (refresh || pixelsPending);
            CaptureTiming timing;
            if (readback || readbackNeedsDrain) {
                traceReadback(readback ? readback->admission.id : physicalTraceId, consumeReady ? DesktopReadbackCheckpointKind::kConsumeReady : DesktopReadbackCheckpointKind::kBeforeAcquire, now);
            }
            if (!consumeReady) {
                auto wait = std::chrono::milliseconds(readyToEncode || !allowEncode || readback ? 0 : 16);
                if (continuousFrames && retained && allowEncode) {
                    wait = std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(std::max(encodeAt - now, VideoClock::duration::zero())));
                }
                pixels = capture->nextFrame(wait, retained ? &retained->bgra : nullptr, &timing, retained.has_value() && fullPixelsValid, readPixels, !encoder || gpu || codecChanged);
            }
            diagnostics.lastJobConsumedReady = consumeReady;
            if (pixels) {
                ++diagnostics.returnedSnapshots;
                diagnostics.lastJobAcquired = true;
                diagnostics.lastAcquireEpoch = pixels->captureEpoch;
                diagnostics.lastAcquireSerial = pixels->sourceSerial;
                diagnostics.lastAcquirePresentTicks = pixels->captureTicks;
            }
            addDuration(stages.acquireWait, timing.stages.acquireWaitMicroseconds);
            addDuration(stages.gpuSyncWait, timing.stages.mapMicroseconds);
            addDuration(stages.prepareCpu, timing.stages.rowCopyMicroseconds);
            cpuMeasuredThisRound = timing.stages.rowCopyMicroseconds.has_value();
            if (pixels && !pixels->bgra.empty()) {
                pendingCpuMap = timing.stages.mapMicroseconds;
                pendingCpuCopy = timing.stages.rowCopyMicroseconds;
            }
            readbackReady = readPixels || gpu;
            diagnostics.captureCalls += !consumeReady;
            if (!pixels && !consumeReady) {
                ++diagnostics.captureTimeouts;
            } else if (pixels && (!pixels->captureTicks || pixels->captureTicks == lastPresentTicks)) {
                ++diagnostics.cursorOnlyUpdates;
            } else if (pixels) {
                ++diagnostics.pixelUpdates;
                const auto capturedAt = std::chrono::steady_clock::now();
                if (previousPixelCaptureAt != std::chrono::steady_clock::time_point{}) {
                    ++diagnostics.pixelCaptureGaps;
                    diagnostics.pixelCaptureGapsGe45ms += capturedAt - previousPixelCaptureAt >= std::chrono::milliseconds(45);
                }
                previousPixelCaptureAt = capturedAt;
            }
        } catch (const DesktopChanged& error) {
            // Never refresh from a snapshot belonging to the lost desktop.
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return std::nullopt;
        }
        const bool repeatDue = continuousFrames && allowEncode && retained && VideoClock::now() >= encodeAt;
        const auto idle = std::chrono::steady_clock::now() - lastContentAt >= std::chrono::milliseconds(180);
        const bool detailDue = allowRefinement && contentFrameId && idle && (optimizationV2 ? detailBudget > 512 && (!refinementPlan.complete() || (readback && readback->purpose == ReadbackPurpose::kStatic)) : refinedFrame != contentFrameId);
        if (!pixels && ((!refresh && !pixelsPending && !detailDue && !repeatDue) || !retained)) {
            return std::nullopt;
        }
        if (pixels) {
            if (pixels->width != damageWidth || pixels->height != damageHeight) {
                damageWidth = pixels->width;
                damageHeight = pixels->height;
                damage.reset(damageWidth, damageHeight);
            }
            const bool newPixels = pixels->captureTicks && pixels->captureTicks != lastPresentTicks;
            if (newPixels) {
                retireWork(DesktopVideoFallbackReason::kSourceReplacement);
                coalescedCaptures += pixelsPending;
                fullPixelsValid = !pixels->bgra.empty();
                if (!fullPixelsValid) {
                    pendingCpuMap.reset();
                    pendingCpuCopy.reset();
                }
                damage.add(pixels->damageKnown, pixels->moves, pixels->dirty);
                diagnostics.unknownDamageUpdates += !pixels->damageKnown;
            }
            pixelsPending = pixelsPending || (pixels->captureTicks != 0 && pixels->captureTicks != lastEncodedTicks);
            if (pixels->captureTicks != 0) {
                lastPresentTicks = pixels->captureTicks;
            }
            if (!pixels->bgra.empty()) {
                fullPixelsValid = true;
            }
            if (pixels->bgra.empty()) {
                if (!retained || retained->width != pixels->width || retained->height != pixels->height ||
                    retained->rotation != pixels->rotation || retained->desktopLeft != pixels->desktopLeft || retained->desktopTop != pixels->desktopTop) {
                    discard();
                    return std::nullopt;
                }
                pixels->bgra = std::move(retained->bgra);
            }
            retained = std::move(pixels);
        }
        const auto& source = *retained;
        const bool sourceChanged = encoder && (source.width != width || source.height != height || source.captureEpoch != configuredCaptureEpoch || source.desktopLeft != configuredLeft || source.desktopTop != configuredTop);
        if (sourceChanged) {
            refresh = true;
            retireWork();
        }
        if (fullPixelsValid && pixelsPending && !cpuMeasuredThisRound) {
            // A complete CPU snapshot may precede the geometry/credit barrier.
            // Report its real used readback once, not a manufactured zero or a
            // repeated old readback on every static refresh.
            addDuration(stages.gpuSyncWait, pendingCpuMap);
            addDuration(stages.prepareCpu, pendingCpuCopy);
            cpuMeasuredThisRound = true;
        }
        if (readback && (readback->tag != source.readbackTag(generation, contentFrameId) || !current(readback->revision))) {
            retireReadback(DesktopVideoFallbackReason::kIdentityOrRevisionMismatch);
        }
        const bool cursorNeedsSource = !fullPixelsValid && DesktopCursor::requiresBackground(source);
        if (encoder && encoder->gpu() && (cursorNeedsSource || (!optimizationV2 && detailDue && !pixelsPending && !refresh && !configurationPending))) {
            std::vector<wire::Rectangle> cursorAreas;
            if (cursorNeedsSource && optimizationV2) {
                const auto left = static_cast<std::uint32_t>(std::clamp<std::int64_t>(source.pointerX, 0, source.width));
                const auto top = static_cast<std::uint32_t>(std::clamp<std::int64_t>(source.pointerY, 0, source.height));
                const auto right = static_cast<std::uint32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(source.pointerX) + source.pointerWidth, 0, source.width));
                const auto bottom = static_cast<std::uint32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(source.pointerY) + (source.pointerType == 1 ? source.pointerHeight / 2 : source.pointerHeight), 0, source.height));
                for (auto y = top; y < bottom; y += 64) {
                    for (auto x = left; x < right; x += 128) {
                        auto& area = cursorAreas.emplace_back();
                        area.set_x(x);
                        area.set_y(y);
                        area.set_width(std::min(128u, right - x));
                        area.set_height(std::min(64u, bottom - y));
                    }
                }
                if (cursorAreas.size() > 32) {
                    cursorAreas.clear(); // Oversized mandatory cursor retains the complete-read path.
                }
            }
            if (!materialize(cursorAreas, cursorNeedsSource ? MaterializeReason::kCursor : MaterializeReason::kLegacyDetail)) {
                return std::nullopt;
            }
        }
        // Fail closed instead of transmitting a rotated or incorrectly cropped
        // desktop. The caller must recreate a supported capture source.
        if (source.rotation != 1 || source.width % 2 || source.height % 2) {
            throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop orientation or dimensions require capture reconfiguration");
        }
        if (source.width != flowWidth || source.height != flowHeight) {
            retireWork();
            geometryChanged = std::pair{source.width, source.height};
            return std::nullopt;
        }
        DesktopVideoUpdate update;
        update.desktopLeft = source.desktopLeft;
        update.desktopTop = source.desktopTop;
        update.sourcePresentTicks = lastPresentTicks;
        update.cursor = cursor.update(source, refresh || configurationPending);
        const auto now = std::chrono::steady_clock::now();
        encodeWindowStarted = true;
        stages.sourceAge = sourceAge(lastPresentTicks);
        stages.coalescedCaptures = coalescedCaptures;
        if (!optimizationV2 && detailDue && !pixelsPending && !refresh && !configurationPending) {
            refinedFrame = contentFrameId;
            auto detail = refineVideoDetails(source.bgra, source.width, source.height, &tiles, capabilities.tile_cache());
            if (detail.patches_size()) {
                update.generation = generation;
                update.frameId = contentFrameId;
                update.refinementFrameId = contentFrameId;
                update.refinement = std::move(detail);
                return update;
            }
        }
        std::vector<DesktopDetailTile> dynamicAreas;
        if (optimizationV2 && encoder && !configurationPending && !refresh && contentFrameId && allowEncode) {
            const auto overlap = std::chrono::duration_cast<std::chrono::microseconds>(std::max(encodeAt, now) - now);
            if (detailDue && !pixelsPending && current(revision)) {
                if (!staticCruise) {
                    refinementPlan.includeCanvas();
                    staticCruise = true;
                }
                const auto budget = std::min(detailBudget, refinementTokens.available(now, detailBudget));
                if (!readback && !readbackChecked && budget > 1024) {
                    detailOverlap = {};
                    detailMinimumWork = {};
                    const auto count = static_cast<std::uint32_t>(std::clamp<std::size_t>((budget - 512) / 2048, 1, 8));
                    // Planner metadata only: upper-bound the upcoming slice's
                    // tile area/count without scanning pixels or advancing it.
                    const std::vector<DesktopDetailTile> predictedAreas(count, {0, 0, std::min(128u, width), std::min(64u, height)});
                    std::size_t selected = count;
                    if (admitOptional(detailCost, now, optionalAllowance(rate), predictedAreas, rate, 0, &selected, budget)) {
                        auto areas = refinementPlan.nextSlice(now, static_cast<std::uint32_t>(selected));
                        if (areas.empty()) {
                            detailCost.finish(VideoClock::now(), true);
                        } else if (fullPixelsValid) {
                            update.refinement = prepareDetail(areas, nullptr, rate, budget, revision, wire::STATIC_DETAIL, detailAdmission, update.refinementAdmission);
                        } else {
                            submitReadback(ReadbackPurpose::kStatic, std::move(areas), refinementPlan.workKey(), revision);
                        }
                    }
                }
                if (readback && readback->purpose == ReadbackPurpose::kStatic && readback->ready) {
                    takeReadbackStages();
                    if (readback->workKey == refinementPlan.workKey()) {
                        update.refinement = prepareDetail(readback->areas, &*readback->ready, rate, budget, revision, wire::STATIC_DETAIL, readback->admission, update.refinementAdmission);
                    } else {
                        detailCost.finish(VideoClock::now(), false);
                    }
                    readback.reset();
                } else if (readback && !detailCost.within(VideoClock::now())) {
                    retireReadback();
                }
                if (update.refinement) {
                    update.generation = generation;
                    update.frameId = contentFrameId;
                    update.baseFrameId = contentFrameId;
                    update.refinementFrameId = contentFrameId;
                    if (!repeatDue) {
                        return update;
                    }
                }
            }
            // A known CPU source may only begin/anchor/claim credit when the
            // generic VIDEO decision below will not return for now < encodeAt.
            // True ROI still submits early for its original copy overlap/D.
            if (pixelsPending && current(revision) && detail::regionPreparationReady(fullPixelsValid, readbackReady, now, encodeAt)) {
                if (!readback && !readbackChecked && !comparisonCost.admitted() && capabilities.region_updates() && comparisonBaseline && encoder->gpu()) {
                    const auto tag = source.readbackTag(generation, contentFrameId);
                    auto baselineContext = *comparisonBaseline;
                    baselineContext.sourceSerial = tag.sourceSerial;
                    const auto candidates = damage.detailCandidates(); // COMPLETE dirty + moveDest union, never truncated.
                    if (baselineContext == tag && tag.sourceSerial >= comparisonBaseline->sourceSerial && !candidates.empty() && candidates.size() <= 32) {
                        comparisonMinimumWork = {};
                        comparisonOverlap = overlap;
                        const auto remaining = regionDecision.remaining(now, encodeAt, primaryAllowance(rate));
                        if (remaining > std::chrono::microseconds::zero() && admitOptional(comparisonCost, now, remaining, candidates, rate, 0, nullptr, 0, DesktopPreparationSource::kRoi, true)) {
                            submitReadback(ReadbackPurpose::kComparison, candidates, std::nullopt, revision);
                        }
                    }
                }
                if (!readback && !readbackChecked && !regionCost.admitted() && capabilities.region_updates() && damage.regionCandidate() && regionByteBudget > 512) {
                    regionOverlap = overlap;
                    regionMinimumWork = {};
                    std::vector<DesktopDetailTile> areas;
                    for (const auto& area : damage.patches()) {
                        areas.push_back({area.x(), area.y(), area.width(), area.height()});
                    }
                    std::size_t copyBytes = 0;
                    for (const auto& copy : damage.copies()) {
                        copyBytes += copy.ByteSizeLong() + 8;
                    }
                    const auto remainingDecision = regionDecision.remaining(now, encodeAt, primaryAllowance(rate));
                    if (remainingDecision > std::chrono::microseconds::zero() && admitOptional(regionCost, now, remainingDecision, areas, rate, copyBytes) && !fullPixelsValid && !areas.empty()) {
                        submitReadback(ReadbackPurpose::kRegion, std::move(areas), std::nullopt, revision);
                    }
                }
                // Draining a retired physical slot already spent this round's
                // probe. Do not submit new work that cannot be checked before
                // its original video decision; the next round owns that chance.
                if (!readback && !readbackChecked && !regionCost.admitted() && allowRefinement && detailBudget > 1024) {
                    auto candidates = damage.detailCandidates();
                    if (!candidates.empty()) {
                        const auto budget = std::min(detailBudget, refinementTokens.available(now, detailBudget));
                        detailOverlap = overlap;
                        detailMinimumWork = {};
                        dynamicCursor %= candidates.size();
                        const auto count = std::min<std::size_t>({8, candidates.size(), std::max<std::size_t>(1, (budget - 512) / 2048)});
                        for (std::size_t index = 0; index < count; ++index) {
                            dynamicAreas.push_back(candidates[(dynamicCursor + index) % candidates.size()]);
                        }
                        std::size_t selected = count;
                        if (budget > 1024 && admitOptional(detailCost, now, overlap + videoEstimate + optionalAllowance(rate), dynamicAreas, rate, 0, &selected, budget)) {
                            dynamicAreas.resize(selected);
                            dynamicCursor = (dynamicCursor + dynamicAreas.size()) % candidates.size();
                            if (!fullPixelsValid) {
                                submitReadback(ReadbackPurpose::kDynamic, std::move(dynamicAreas), std::nullopt, revision);
                            }
                        } else {
                            dynamicAreas.clear();
                        }
                    }
                }
            }
        }
        if (readback && primaryReadback(readback->purpose) && !readback->ready &&
            timerResolution.status().active && requestedV2 && capabilities.region_updates() && allowEncode && !refresh && !configurationPending &&
            current(readback->revision) && readback->tag == source.readbackTag(generation, contentFrameId) &&
            regionDecision.pending(VideoClock::now()) && readbackCost().within(VideoClock::now())) {
            // Policy B: E is only the earliest VIDEO start. A valid Pending
            // REGION may use the remainder of its original anchored D, with
            // one nonblocking probe on each normal bounded timer dispatch.
            ++diagnostics.regionDecisionWaits;
            return update.cursor ? std::optional{std::move(update)} : std::nullopt;
        }
        if (readback && readback->purpose == ReadbackPurpose::kComparison && readback->ready && !readback->proofApplied) {
            const auto previous = damage.changedPixels();
            takeReadbackStages();
            if (applyComparison()) {
                diagnostics.filteredPixels += previous - std::min(previous, damage.changedPixels());
                if (damage.unchanged()) {
                    ++diagnostics.comparison.unchanged;
                    readback.reset();
                    consumeUnchanged();
                    if (!continuousFrames) {
                        // No primary output in the legacy change-driven path.
                        return update.cursor ? std::optional{std::move(update)} : std::nullopt;
                    }
                }
            }
        }
        if (encoder && !configurationPending && ((!pixelsPending && !refresh && !repeatDue) || (!allowEncode && !sourceChanged && !contractChanged) || (!readbackReady && !sourceChanged && !contractChanged) || (!refresh && now < encodeAt))) {
            if (!allowEncode) {
                ++diagnostics.backpressuredCalls;
            }
            if (!refresh && pixelsPending && now < encodeAt) {
                ++diagnostics.pendingBeforeDeadlineCalls;
            }
            if (update.cursor || update.refinement) {
                return update;
            }
            return std::nullopt;
        }
        if (optimizationV2 && encoder && !configurationPending && !refresh && contentFrameId && capabilities.region_updates() && damage.regionCandidate()) {
            const bool compared = readback && readback->purpose == ReadbackPurpose::kComparison && readback->ready && readback->proofApplied;
            if (compared) {
                const auto preparationStarted = VideoClock::now();
                const auto remaining = regionDecision.remaining(preparationStarted, encodeAt, primaryAllowance(rate));
                std::vector<DesktopDetailTile> patches;
                for (const auto& area : damage.patches()) {
                    patches.push_back({area.x(), area.y(), area.width(), area.height()});
                }
                std::size_t copyBytes = 0;
                for (const auto& copy : damage.copies()) {
                    copyBytes += copy.ByteSizeLong() + 8;
                }
                regionOverlap = {}; // Packed CPU PNG has no comparison/pacer/codec overlap.
                regionMinimumWork = {};
                DesktopComparisonPngDiagnostic* pngEvidence = nullptr;
                if (diagnostics.lastReadback && diagnostics.lastReadback->admission.id == readback->admission.id) {
                    pngEvidence = &diagnostics.lastReadback->png.emplace();
                }
                admitOptional(regionCost, preparationStarted, remaining, patches, rate, copyBytes, nullptr, 0,
                              DesktopPreparationSource::kCpu, false, readback->comparisonPrefix, pngEvidence);
            }
            const bool ready = readback && (readback->purpose == ReadbackPurpose::kRegion || compared) && readback->ready;
            if (regionCost.admitted() && (!readback || ready)) {
                ++diagnostics.regionCandidates;
                bool unchanged = false;
                if (ready) {
                    takeReadbackStages();
                }
                auto region = prepareRegion(rate, regionByteBudget, revision, ready ? &*readback->ready : nullptr, unchanged);
                readback.reset();
                if (unchanged) {
                    consumeUnchanged();
                    if (!continuousFrames) {
                        return update.cursor ? std::optional{std::move(update)} : std::nullopt;
                    }
                }
                if (region) {
                    if (deliveryFrameId == std::numeric_limits<std::uint64_t>::max()) {
                        throw std::overflow_error("Desktop frame sequence exhausted");
                    }
                    if (!rememberPixels()) {
                        return std::nullopt;
                    }
                    update.generation = generation;
                    update.baseFrameId = deliveryFrameId;
                    update.frameId = ++deliveryFrameId;
                    contentFrameId = deliveryFrameId;
                    commitComparisonBaseline();
                    region->set_encode_microseconds(static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - now).count() + earlyOptionalCompute.count(), 1, 60000000)));
                    writeStages(*region->mutable_stage_timing(), stages);
                    update.regionAdmission = DesktopPrimaryAdmission{regionAdmission, generation, contentFrameId, update.baseFrameId, jobId};
                    update.changedPixels = 0;
                    for (const auto& patch : region->patches()) {
                        update.changedPixels += static_cast<std::uint64_t>(patch.destination().width()) * patch.destination().height();
                    }
                    update.region = std::move(region);
                    regionDecision.reset();
                    pixelsPending = false;
                    lastEncodedTicks = lastPresentTicks;
                    lastEncodedAt = VideoClock::now();
                    lastContentAt = lastEncodedAt;
                    encodeAt = nextDesktopFrameAt(encodeAt, cadenceStarted, rate.framesPerSecond, continuousFrames, false);
                    damage.commit();
                    coalescedCaptures = 0;
                    pendingCpuMap.reset();
                    pendingCpuCopy.reset();
                    refinementPlan.begin(generation, contentFrameId, width, height, VideoClock::now());
                    return update;
                }
            } else if (readback && primaryReadback(readback->purpose)) {
                // No late-wake grace and no moving begin(): at original D (or
                // unavailable timer request) Pending falls back to VIDEO.
                const auto decisionCheckedAt = VideoClock::now();
                const bool decisionExpired = !regionDecision.pending(decisionCheckedAt);
                if (decisionExpired) {
                    traceReadback(readback->admission.id, DesktopReadbackCheckpointKind::kDecisionExpired, decisionCheckedAt);
                    ++diagnostics.regionDecisionExpirations;
                }
                retireReadback(decisionExpired ? DesktopVideoFallbackReason::kDecisionWindowEnded : DesktopVideoFallbackReason::kNoReadyOrTimer);
            }
        }
        if (readback && readback->purpose == ReadbackPurpose::kComparison) {
            retireReadback(DesktopVideoFallbackReason::kPrepareRejected); // Partial/late/no eligible PNG always VIDEO, never a false skip.
        }
        if (!optimizationV2 && encoder && !configurationPending && !refresh && contentFrameId && capabilities.region_updates() &&
            (damage.regionCandidate() || now >= compareAt)) {
            const auto previousPixels = damage.changedPixels();
            if (!comparePixels()) {
                return std::nullopt;
            }
            diagnostics.filteredPixels += previousPixels - std::min(previousPixels, damage.changedPixels());
            // Large moving scenes already require video. Reprobe their damage
            // after 100 ms instead of synchronizing a GPU comparison every frame.
            compareAt = damage.unchanged() || damage.regionCandidate() ? now : now + std::chrono::milliseconds(100);
            if (damage.unchanged()) {
                ++diagnostics.unchangedPixelUpdates;
                pixelsPending = false;
                lastEncodedTicks = lastPresentTicks;
                damage.commit();
                return update.cursor ? std::optional{std::move(update)} : std::nullopt;
            }
        }
        update.changedPixels = damage.changedPixels();
        if (!optimizationV2 && encoder && !configurationPending && !refresh && contentFrameId && capabilities.region_updates() && damage.regionCandidate()) {
            ++diagnostics.regionCandidates;
            regionBudget.begin(std::chrono::steady_clock::now(), rate.framesPerSecond);
            const auto patches = damage.patches();
            if (encoder->gpu() && !patches.empty() && !materialize(patches)) {
                return std::nullopt;
            }
            wire::DesktopRegionUpdate region;
            for (const auto& copy : damage.copies()) {
                *region.add_copies() = copy;
            }
            tiles.begin();
            std::size_t bytes = 0;
            bool fits = true;
            // Spend at most half a video-frame budget on PNGs. Cached copies
            // remain cheap; high-entropy updates go directly to the video codec.
            const auto budget = std::min<std::size_t>(256 * 1024, std::max<std::size_t>(4096, rate.bitrate / rate.framesPerSecond / 16));
            for (const auto& area : patches) {
                auto patch = tiles.prepare(source.bgra, source.width, source.height, area, capabilities.tile_cache(), budget - bytes, &regionBudget);
                if (!patch) {
                    fits = false;
                    break;
                }
                bytes += patch->png().size();
                *region.add_patches() = std::move(*patch);
            }
            const bool withinBudget = regionBudget.within(std::chrono::steady_clock::now());
            fits &= withinBudget;
            if (!withinBudget || regionBudget.denied()) {
                ++diagnostics.regionTimeBudgetFailures;
            }
            if (fits) {
                if (deliveryFrameId == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("Desktop frame sequence exhausted");
                }
                if (!rememberPixels()) {
                    return std::nullopt;
                }
                tiles.commit();
                region.set_encode_microseconds(static_cast<std::uint32_t>(std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now).count())));
                region.set_target_frames_per_second(rate.framesPerSecond);
                region.set_target_bitrate(rate.bitrate);
                update.generation = generation;
                update.baseFrameId = deliveryFrameId;
                update.frameId = ++deliveryFrameId;
                contentFrameId = deliveryFrameId;
                update.changedPixels = 0;
                for (const auto& patch : region.patches()) {
                    update.changedPixels += static_cast<std::uint64_t>(patch.destination().width()) * patch.destination().height();
                }
                update.region = std::move(region);
                pixelsPending = false;
                lastEncodedTicks = lastPresentTicks;
                lastEncodedAt = std::chrono::steady_clock::now();
                lastContentAt = lastEncodedAt;
                encodeAt = nextDesktopFrameAt(encodeAt, cadenceStarted, rate.framesPerSecond, continuousFrames, false);
                damage.commit();
                return update;
            }
            tiles.begin();
            ++diagnostics.regionBudgetFailures;
        }
        if (!encoder || sourceChanged || codecChanged || contractChanged) {
            regionDecision.reset();
            clearComparisonBaseline();
            encoder = std::make_unique<VideoEncoder>(VideoEncoderConfig{source.width, source.height, rate.bitrate, rate.framesPerSecond, rate.sharp, codecs}, capture->device());
            requestedCodecs = std::move(codecs);
            optimizationV2 = requestedV2;
            continuousFrames = requestedContinuous;
            retireWork();
            width = source.width;
            height = source.height;
            configuredCaptureEpoch = source.captureEpoch;
            configuredLeft = source.desktopLeft;
            configuredTop = source.desktopTop;
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Desktop generation exhausted");
            }
            ++generation;
            contentFrameId = 0;
            deliveryFrameId = 0;
            refinedFrame = 0;
            configurationPending = true;
            tiles.clear();
        }
        encoder->setRate(rate.framesPerSecond, rate.bitrate);
        encoder->setDetail(rate.sharp);
        if ((!fullPixelsValid || (!optimizationV2 && capturedOnGpu)) && !encoder->gpu() && !materialize()) {
            return std::nullopt;
        }
        // Acquisition/readback belong to this frame's period. Starting the
        // clock after a slow Map would add another idle wait to every frame.
        encodeAt = nextDesktopFrameAt(encodeAt, cadenceStarted, rate.framesPerSecond, continuousFrames, refresh || configurationPending);
        bool repeated = continuousFrames && contentFrameId && !pixelsPending && !refresh && !configurationPending;
        const auto encodeStarted = std::chrono::steady_clock::now();
        if (diagnostics.lastReadback && diagnostics.lastReadback->retirementReason != DesktopVideoFallbackReason::kNone && diagnostics.lastReadback->videoFallbackReason == DesktopVideoFallbackReason::kNone) {
            auto& trace = *diagnostics.lastReadback;
            trace.videoFallbackReason = trace.retirementReason;
            trace.fallbackJobId = jobId;
            trace.fallbackSourceSerial = source.sourceSerial;
            traceReadback(trace.admission.id, DesktopReadbackCheckpointKind::kVideoFallback, encodeStarted);
        }
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(encodeStarted - epoch).count();
        const bool gpuEncoding = encoder->gpu();
        ++diagnostics.videoEncodeCalls;
        auto encoded = gpuEncoding ? encoder->encodeTexture(capture->texture(), static_cast<std::uint64_t>(timestamp), refresh || configurationPending)
                                   : encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(timestamp), refresh || configurationPending);
        optionalEncoderTime = std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - encodeStarted);
        if (!encoded && encoded.error() == VideoEncodeError::kGpuUnavailable) {
            if ((!optimizationV2 || !fullPixelsValid) && !materialize({}, MaterializeReason::kGpuFallback)) {
                return std::nullopt;
            }
            const auto fallbackStarted = VideoClock::now();
            ++diagnostics.videoEncodeCalls;
            encoded = encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(timestamp), true);
            optionalEncoderTime += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - fallbackStarted);
        }
        if (comparisonRetired) {
            comparisonOverlap += optionalEncoderTime; // Actual codec interval after E; disjoint from the initial pacer segment.
        }
        if (optimizationV2) {
            stages.videoEncode = static_cast<std::uint32_t>(std::clamp<std::int64_t>(optionalEncoderTime.count(), 0, 60000000));
        }
        // Include rejected region work and GPU comparison in encoder pressure.
        auto encodeMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now).count() + (optimizationV2 ? earlyOptionalCompute.count() : 0);
        if (!encoded) {
            if (encoded.error() == VideoEncodeError::kFrameSkipped) {
                if (!repeated) {
                    retireWork();
                }
                if (update.cursor || update.refinement) {
                    return update;
                }
                return std::nullopt;
            }
            retireWork();
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop video encoding failed");
        }
        if ((refresh || configurationPending) && !encoded->keyFrame) {
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop refresh did not produce a key frame");
        }
        if (encoded->baselineReset) {
            regionDecision.reset();
            if (!encoded->keyFrame || generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("Cannot rebuild desktop encoder baseline");
            }
            ++generation;
            contentFrameId = 0;
            deliveryFrameId = 0;
            repeated = false;
            update.refinement.reset();
            update.refinementAdmission.reset();
            update.refinementFrameId = 0;
            refinedFrame = 0;
            configurationPending = true;
            retireWork();
            tiles.clear();
            update.cursor = cursor.update(source, true);
        }
        // Video encoding is the natural overlap already included in dynamic's
        // budget. Probe only once, after baselineReset has rejected old work,
        // while the source texture and composed base still match the submit.
        if (readback && readback->purpose == ReadbackPurpose::kDynamic) {
            const auto completionStarted = VideoClock::now();
            if (readback->tag == source.readbackTag(generation, contentFrameId) && current(readback->revision) && detailCost.within(VideoClock::now())) {
                checkReadback();
            }
            if (readback && !readback->ready) {
                retireReadback();
            }
            encodeMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - completionStarted).count();
        }
        if (deliveryFrameId == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop frame sequence exhausted");
        }
        if (!repeated && !rememberPixels()) {
            return std::nullopt;
        }
        update.generation = generation;
        pixelsPending = false;
        const auto previousContentFrame = contentFrameId;
        update.frameId = ++deliveryFrameId;
        update.baseFrameId = repeated ? contentFrameId : 0;
        if (!repeated) {
            contentFrameId = deliveryFrameId;
            lastContentAt = VideoClock::now();
            commitComparisonBaseline();
        }
        lastEncodedTicks = lastPresentTicks;
        lastEncodedAt = std::chrono::steady_clock::now();
        if (optimizationV2 && !repeated) {
            const auto detailStarted = VideoClock::now();
            refinementPlan.begin(generation, contentFrameId, width, height, VideoClock::now());
            if (readback && readback->purpose == ReadbackPurpose::kDynamic && readback->ready) {
                if (readback->tag == source.readbackTag(generation, previousContentFrame) && current(readback->revision)) {
                    takeReadbackStages();
                    dynamicAreas = readback->areas;
                } else {
                    retireReadback();
                }
            }
            if (!dynamicAreas.empty() && detailCost.admitted() && allowRefinement && current(revision) && refinementPlan.reserveTiles(dynamicAreas)) {
                const auto budget = std::min(detailBudget, refinementTokens.available(VideoClock::now(), detailBudget));
                update.refinement = prepareDetail(dynamicAreas, readback && readback->ready ? &*readback->ready : nullptr, rate, budget, revision, wire::DYNAMIC_DETAIL, readback && readback->ready ? readback->admission : detailAdmission, update.refinementAdmission);
                update.refinementFrameId = contentFrameId;
                readback.reset();
            } else if (readback) {
                retireReadback();
            }
            if (detailCost.admitted()) {
                detailCost.finish(VideoClock::now(), false, optionalEncoderTime + detailOverlap, detailMinimumWork);
            }
            videoEstimate = (videoEstimate * 3 + optionalEncoderTime) / 4;
            lastVideoBytes = encoded->annexB.size();
            encodeMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - detailStarted).count();
        }
        damage.commit();
        coalescedCaptures = 0;
        pendingCpuMap.reset();
        pendingCpuCopy.reset();
        if (configurationPending) {
            update.configuration.emplace();
            update.configuration->set_codec(encoded->codec);
            update.configuration->set_desktop_optimization(true);
            update.configuration->set_desktop_optimization_contract(2); // Support, not activation without both gates.
            update.configuration->set_continuous_frames(continuousFrames);
            update.configuration->set_width(width);
            update.configuration->set_height(height);
            for (const auto& mode : capture->displayModes()) {
                auto* advertised = update.configuration->add_display_modes();
                advertised->set_width(mode.width);
                advertised->set_height(mode.height);
            }
            configurationPending = false;
        }
        update.frame.set_data(encoded->annexB.data(), encoded->annexB.size());
        update.frame.set_key_frame(encoded->keyFrame);
        update.frame.set_repeated(repeated);
        update.frame.set_timestamp_microseconds(encoded->timestampMicroseconds);
        encodeMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(VideoClock::now() - now).count() + (optimizationV2 ? earlyOptionalCompute.count() : 0);
        update.frame.set_encode_microseconds(static_cast<std::uint32_t>(std::clamp<std::int64_t>(encodeMicroseconds, 1, std::numeric_limits<std::uint32_t>::max())));
        update.frame.set_hardware_encoding(encoder->hardware());
        update.frame.set_gpu_encoding(gpuEncoding && encoder->gpu());
        update.frame.set_target_frames_per_second(rate.framesPerSecond);
        update.frame.set_target_bitrate(rate.bitrate);
        if (optimizationV2) {
            writeStages(*update.frame.mutable_stage_timing(), stages);
        }
        regionDecision.reset();
        ++diagnostics.encodedFrames;
        return update;
    } catch (const DesktopChanged& error) {
        // Covers optional submit/completion too, not just Acquire/materialize.
        // No lost-source ROI, cache transaction or retained pixels may survive.
        discard();
        if (!recovery.restart(VideoClock::now())) {
            throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
        }
        return std::nullopt;
    }
};
} // namespace
struct DesktopVideoRuntime::State : std::enable_shared_from_this<State> {
    ruvia::execution_handle loop;
    ruvia::blocking_pool pool{{.thread_count_ = 1, .queue_capacity_ = 1}};
    std::shared_ptr<VideoWorker> worker = std::make_shared<VideoWorker>();
    WindowsDeadlineTimer pace;
    WindowsDeadlineTimer releaseWait;
    std::shared_future<std::pair<DesktopVideoDiagnostics, bool>> releaseCompletion;
    std::chrono::steady_clock::time_point nextFrameAt{};
    std::uint32_t display;
    const std::uint32_t serverOptimizationContract;
    const bool serverContinuousFrames;
    std::uint32_t pendingOptimizationContract;
    bool optimizationActive = false;
    std::uint64_t retirementSerial = 0;
    std::optional<std::pair<DesktopDetailBatchKey, DesktopDetailAck>> refinementAcknowledgement;
    std::optional<DesktopDetailBatchKey> rejectedPublication;
    std::optional<VideoClock::time_point> creditClosedAt;
    std::uint32_t lastCreditWaitMicroseconds = 0, lastBlockingFlags = 0;
    bool stopped = false, pending = false;
    bool reset = false;
    bool baselineRequired = true;
    bool qualityRefresh = false;
    bool refinementEnabled = false;
    wire::DesktopVideoCapabilities capabilities;
    DesktopFrameFlow flow;
    DesktopVideoDiagnostics diagnostics;
    std::uint64_t publishedVideoFrames = 0, rejectedPrivateVideoFrames = 0, rejectedPrivateJobs = 0;
    std::optional<DesktopRefinementAdmission> latestPublishedRefinement;
    std::optional<DesktopPrimaryAdmission> latestPublishedRegion;

    void takeWorkerDiagnostics(DesktopVideoDiagnostics snapshot) {
        snapshot.publishedVideoFrames = publishedVideoFrames;
        snapshot.rejectedPrivateVideoFrames = rejectedPrivateVideoFrames;
        snapshot.rejectedPrivateJobs = rejectedPrivateJobs;
        snapshot.ownerRevision = worker->requestedRevision.load();
        snapshot.latestPublishedRefinement = latestPublishedRefinement;
        snapshot.latestPublishedRegion = latestPublishedRegion;
        diagnostics = std::move(snapshot);
    }
    void rejectPrivate(const std::optional<DesktopVideoUpdate>& result, bool& counted) {
        if (result && (!result->frame.data().empty() || result->region)) {
            // Durable existing reset path: suspend/release must not clear it.
            // Only the next worker dispatch consuming reset can use comparison.
            reset = true;
            baselineRequired = true;
        }
        if (counted) {
            return;
        }
        counted = true;
        ++rejectedPrivateJobs;
        rejectedPrivateVideoFrames += result && !result->frame.data().empty();
        diagnostics.rejectedPrivateJobs = rejectedPrivateJobs;
        diagnostics.rejectedPrivateVideoFrames = rejectedPrivateVideoFrames;
    }
    State(ruvia::execution_handle owner, std::uint32_t index, std::uint32_t serverContract, bool continuous) : loop(std::move(owner)), pace(loop.executor()), releaseWait(loop.executor()), display(index), serverOptimizationContract(serverContract == 2 ? 2 : 0), serverContinuousFrames(continuous && serverContract == 2), pendingOptimizationContract(serverOptimizationContract) {
        if (display >= 64) {
            throw std::invalid_argument("Desktop display index exceeds limit");
        }
    }
    ~State() {
        if (releaseCompletion.valid()) {
            releaseCompletion.wait(); // Shutdown only, never the event-loop hot path.
        }
        // A final owned job releases the thread-affine capture/codec objects;
        // join ensures no encoding worker survives the runtime.
        std::promise<void> released;
        auto completion = released.get_future();
        if (pool.submit([owned = std::move(worker), released = std::move(released)]() mutable { owned.reset(); released.set_value(); }) != ruvia::blocking_submit_status::accepted) {
            std::terminate();
        }
        // join() stops the pool and can discard queued jobs, so the final
        // resource-release job must finish first.
        completion.get();
        pool.join();
    }
    void releaseTimer() {
        if (releaseCompletion.valid() && releaseCompletion.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            return;
        }
        std::promise<std::pair<DesktopVideoDiagnostics, bool>> released;
        releaseCompletion = released.get_future().share();
        if (pool.submit([owned = worker, released = std::move(released), owner = weak_from_this(), executor = loop.executor()]() mutable {
                owned->suspend();
                released.set_value({owned->diagnostics, !owned->encoder});
                asio::post(executor, [owner] {
                    if (const auto self = owner.lock()) {
                        self->releaseWait.cancel();
                    }
                });
            }) != ruvia::blocking_submit_status::accepted) {
            throw std::logic_error("Desktop timer release could not be queued");
        }
    }
    static asio::awaitable<void> awaitTimerRelease(std::shared_ptr<State> self) {
        while (self->releaseCompletion.valid() && self->releaseCompletion.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            self->releaseWait.expiresAt(VideoClock::time_point::max());
            std::error_code error;
            co_await self->releaseWait.asyncWait(asio::redirect_error(asio::use_awaitable, error));
            if (error && error != asio::error::operation_aborted) {
                throw std::system_error(error);
            }
        }
        if (self->releaseCompletion.valid()) {
            const auto& released = self->releaseCompletion.get();
            self->takeWorkerDiagnostics(released.first);
            self->baselineRequired = released.second;
            if (self->baselineRequired) {
                self->optimizationActive = false;
            }
            self->releaseCompletion = {};
        }
    }
    bool requestsOptimization() const {
        return serverOptimizationContract == 2 && pendingOptimizationContract == 2 && capabilities.desktop_optimization_contract() == 2;
    }
    bool requestsContinuousFrames() const {
        return serverContinuousFrames && requestsOptimization() && capabilities.continuous_frames();
    }
    static asio::awaitable<std::optional<DesktopVideoUpdate>> nextOwned(std::shared_ptr<State> self, bool refresh) {
        if (self->pending) {
            throw std::logic_error("Concurrent desktop video reads are not allowed");
        }
        struct Pending {
            bool& value;
            ~Pending() { value = false; }
        } guard{self->pending};
        self->pending = true;
        co_await awaitTimerRelease(self);
        if (self->stopped) {
            co_return std::nullopt;
        }
        if (self->flow.background()) {
            // Hidden workspaces need neither capture nor pixel copies. A
            // visibility ACK wakes this timer immediately; the bounded wait
            // also lets the owner process pending resize/rebind work.
            self->pace.expiresAfter(std::chrono::milliseconds(250));
        } else {
            self->pace.expiresAt(self->nextFrameAt);
        }
        const auto pacingStarted = VideoClock::now();
        const auto plannedWake = !self->flow.background() && self->nextFrameAt != VideoClock::time_point{} ? std::optional(self->nextFrameAt) : std::nullopt;
        std::error_code error;
        co_await self->pace.asyncWait(asio::redirect_error(asio::use_awaitable, error));
        if (self->stopped || self->flow.background()) {
            self->releaseTimer();
            co_await awaitTimerRelease(self);
            co_return std::nullopt;
        }
        if (error && error != asio::error::operation_aborted) {
            throw std::system_error(error);
        }
        const auto now = std::chrono::steady_clock::now();
        const bool allowEncode = self->reset || self->baselineRequired || self->flow.canEncode(now);
        const bool qualityRefresh = std::exchange(self->qualityRefresh, false);
        refresh = refresh || qualityRefresh;
        const auto [flowWidth, flowHeight] = self->flow.geometry();
        const bool requestedV2 = self->requestsOptimization();
        const bool requestedContinuous = self->requestsContinuousFrames();
        const bool allowRefinement = self->refinementEnabled && self->flow.detailAllowed() && (requestedV2 || !self->flow.pendingFrames());
        const auto detailBudget = allowRefinement && requestedV2 ? self->flow.refinementBudget(now, std::chrono::milliseconds(50)) : 0;
        const auto remaining = self->flow.byteWindow() - std::min(self->flow.byteWindow(), self->flow.pressureBytes(now));
        const auto rate = self->flow.rate();
        const auto regionByteBudget = std::min({std::size_t{256 * 1024}, remaining, static_cast<std::size_t>(rate.bitrate / 160),
                                                std::max<std::size_t>(512, rate.bitrate / rate.framesPerSecond / 16)});
        DesktopStageDurations initialStages;
        initialStages.pacingCreditWait = DesktopStageDurations::microseconds(pacingStarted, now);
        if (!allowEncode && !self->creditClosedAt) {
            self->creditClosedAt = now;
        }
        if (!allowEncode) {
            self->lastBlockingFlags |= self->flow.snapshot(now).blockingFlags;
        }
        if (allowEncode && self->creditClosedAt) {
            initialStages.pacingCreditWait = DesktopStageDurations::microseconds(*self->creditClosedAt, now);
            self->lastCreditWaitMicroseconds = *initialStages.pacingCreditWait;
            self->creditClosedAt.reset();
        }
        const auto queuedAt = VideoClock::now();
        const auto revision = self->worker->requestedRevision.load();
        auto operation = ruvia::run_blocking(self->pool, self->loop, [owned = self->worker, display = self->display, refresh, reset = std::exchange(self->reset, false), rate, flowWidth, flowHeight, allowEncode, allowRefinement, capabilities = self->capabilities, requestedV2, requestedContinuous, detailBudget, regionByteBudget, acknowledgement = std::exchange(self->refinementAcknowledgement, std::nullopt), revision, initialStages, queuedAt, rejectedPublication = std::exchange(self->rejectedPublication, std::nullopt)]() mutable {
            const auto computeStarted = VideoClock::now();
            initialStages.workerQueue = DesktopStageDurations::microseconds(queuedAt, computeStarted);
            std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged;
            auto update = owned->next(display, refresh, reset, rate, flowWidth, flowHeight, geometryChanged, allowEncode, allowRefinement, capabilities,
                                      requestedV2, requestedContinuous, detailBudget, regionByteBudget, acknowledgement, revision, initialStages, rejectedPublication);
            const auto now = std::chrono::steady_clock::now();
            if (update) {
                update->workerDispatchMicroseconds = DesktopStageDurations::microseconds(computeStarted, now);
            }
            // Cursor metadata remains responsive without repeatedly mapping
            // pixels during a closed receiver window. ACKs wake this wait.
            auto wakeAt = owned->readback ? now + std::chrono::milliseconds(1) : allowEncode && owned->encodeAt > now ? std::min(owned->encodeAt, now + std::chrono::milliseconds(owned->optimizationV2 ? 1 : 33))
                                                                                                                      : now + std::chrono::milliseconds(allowEncode ? 1 : 33);
            if (owned->continuousFrames && !owned->pixelsPending && !owned->readback && allowEncode && owned->encodeAt > now) {
                wakeAt = owned->encodeAt;
            }
            if (owned->readback && VideoWorker::primaryReadback(owned->readback->purpose) && owned->regionDecision.deadline()) {
                wakeAt = std::min(wakeAt, *owned->regionDecision.deadline());
            }
            auto completed = std::tuple{std::move(update), std::max(owned->recovery.nextAttempt(), wakeAt), geometryChanged, owned->diagnostics, !owned->encoder, owned->optimizationV2, owned->retirementSerial};
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
            detail::beforeDesktopJobReturnFixture(std::get<0>(completed), std::get<3>(completed));
#endif
            return completed;
        });
        auto [result, retryAt, geometryChanged, diagnostics, baselineRequired, activeV2, retirementSerial] = (co_await ruvia::as_awaitable(std::move(operation))).value();
        if (retirementSerial != self->retirementSerial) {
            self->retirementSerial = retirementSerial;
            self->flow.retireRefinement(VideoClock::now()); // Retire validity, NEVER unacked credit.
        }
        self->takeWorkerDiagnostics(diagnostics);
        bool privateRejected = false;
        self->baselineRequired = baselineRequired;
        if (self->stopped || self->flow.background()) {
            self->rejectPrivate(result, privateRejected);
            self->releaseTimer();
            co_await awaitTimerRelease(self);
            co_return std::nullopt; // Even a foreground transition during release cannot publish this rejected job.
        }
        if (qualityRefresh && (!result || result->frame.data().empty())) {
            self->qualityRefresh = true;
        }
        self->nextFrameAt = retryAt; // Only capture recovery inserts a retry delay.
        if (self->worker->requestedRevision.load() != revision) {
            // The worker may have advanced its private baseline while the owner
            // changed permission. Reject the whole old job, then rebuild; merely
            // stripping detail would leave an unsent composed/reference frame.
            if (result && (!result->frame.data().empty() || result->region)) {
                self->rejectPrivate(result, privateRejected);
            } else {
                // Rejected auxiliary output cannot roll back the already accepted primary.
                if (result && result->refinement && result->refinement->batch_id()) {
                    self->rejectedPublication = DesktopDetailBatchKey{result->generation, result->refinementFrameId, result->refinement->batch_id()};
                }
                self->rejectPrivate(result, privateRejected);
                co_return std::nullopt;
            }
        }
        if (self->stopped || self->reset) {
            self->rejectPrivate(result, privateRejected);
            co_return std::nullopt;
        }
        if (geometryChanged) {
            self->rejectPrivate(result, privateRejected);
            if (!self->flow.setGeometry(geometryChanged->first, geometryChanged->second)) {
                throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop dimensions exceed the encoder quality-budget limits");
            }
            self->baselineRequired = true;
            co_return std::nullopt;
        }
        if (result && result->configuration) {
            const bool stillRequested = self->requestsOptimization();
            if (activeV2 != stillRequested) {
                self->rejectPrivate(result, privateRejected);
                self->reset = true;
                self->baselineRequired = true;
                co_return std::nullopt; // Unsent stale Config; rebuild another generation barrier.
            }
            self->optimizationActive = activeV2;
        }
        if (result && (!result->frame.data().empty() || result->region)) {
            const auto snapshot = self->flow.snapshot(VideoClock::now());
            auto* sample = &result->flowSample.emplace();
            sample->set_frame_window(snapshot.frameWindow);
            sample->set_byte_window(snapshot.byteWindow);
            sample->set_pending_frames(snapshot.pendingFrames);
            sample->set_pending_bytes(snapshot.pendingBytes);
            sample->set_pressure_bytes(snapshot.pressureBytes);
            sample->set_path_delay_microseconds(snapshot.pathDelayMicroseconds);
            sample->set_oldest_frame_age_microseconds(snapshot.oldestFrameAgeMicroseconds);
            sample->set_delivery_bytes_per_second(snapshot.deliveryBytesPerSecond);
            sample->set_worker_dispatch_microseconds(result->workerDispatchMicroseconds);
            sample->set_credit_wait_microseconds(std::exchange(self->lastCreditWaitMicroseconds, 0));
            sample->set_blocking_flags(snapshot.blockingFlags | std::exchange(self->lastBlockingFlags, 0));
            if (plannedWake && error != asio::error::operation_aborted) {
                sample->set_owner_loop_late_microseconds(DesktopStageDurations::microseconds(*plannedWake, now));
            }
            const auto bytes = result->region ? result->region->ByteSizeLong() : result->frame.ByteSizeLong();
            const auto type = result->region              ? DesktopFrameFlow::FrameType::kRegion
                              : result->frame.repeated()  ? (result->frame.key_frame() ? DesktopFrameFlow::FrameType::kRepeatedKeyFrame : DesktopFrameFlow::FrameType::kRepeatedVideo)
                              : result->frame.key_frame() ? DesktopFrameFlow::FrameType::kKeyFrame
                                                          : DesktopFrameFlow::FrameType::kVideo;
            self->flow.observeFrame(type, bytes, result->changedPixels, std::chrono::microseconds(result->region ? result->region->encode_microseconds() : result->frame.encode_microseconds()));
            self->flow.sent(result->generation, result->frameId, std::chrono::steady_clock::now(), bytes, type);
            if (!result->frame.data().empty()) {
                self->diagnostics.publishedVideoFrames = ++self->publishedVideoFrames;
            } else {
                self->latestPublishedRegion = result->regionAdmission;
                self->diagnostics.latestPublishedRegion = self->latestPublishedRegion;
            }
        }
        if (result && result->refinement && result->refinement->batch_id()) {
            const DesktopDetailBatchKey key{result->generation, result->refinementFrameId, result->refinement->batch_id()};
            // Owner-loop Flow is the authoritative single-unacked credit owner.
            // The planner may have retired its old computation, not this lease.
            if (!self->optimizationActive || !self->refinementEnabled || self->worker->requestedRevision.load() != revision ||
                !self->flow.sentRefinement(key.generation, key.frame, key.batch, VideoClock::now(), result->refinement->ByteSizeLong() + 512, std::chrono::milliseconds(50))) {
                self->rejectedPublication = key;
                result->refinement.reset();
                result->refinementAdmission.reset();
                if (result->frame.data().empty() && !result->region && !result->cursor) {
                    result.reset();
                }
            } else {
                self->latestPublishedRefinement = result->refinementAdmission;
                self->diagnostics.latestPublishedRefinement = self->latestPublishedRefinement;
            }
        }
        co_return result;
    }
    static asio::awaitable<bool> resizeOwned(std::shared_ptr<State> self, std::uint32_t width, std::uint32_t height) {
        if (self->pending) {
            throw std::logic_error("Concurrent desktop capture operations are not allowed");
        }
        if (self->stopped || width < 16 || height < 16 || width > 4096 || height > 2160 || width % 2 || height % 2) {
            co_return false;
        }
        struct Pending {
            bool& value;
            ~Pending() { value = false; }
        } guard{self->pending};
        self->pending = true;
        auto operation = ruvia::run_blocking(self->pool, self->loop, [owned = self->worker, width, height] {
            try {
                if (!owned->capture || !owned->capture->resize(width, height)) {
                    return false;
                }
                owned->discard();
                // An accepted user-requested mode change starts a new recovery
                // episode. Successful frames alone never reset a flapping
                // device's budget, but earlier modes must not penalize this one.
                owned->recovery = CaptureRecovery{};
                return true;
            } catch (const DesktopChanged&) {
                owned->discard();
                return false;
            }
        });
        const bool applied = (co_await ruvia::as_awaitable(std::move(operation))).value();
        if (applied) {
            self->nextFrameAt = {};
            self->baselineRequired = true;
        }
        co_return applied;
    }
};
DesktopVideoRuntime::DesktopVideoRuntime(ruvia::execution_handle loop, std::uint32_t displayIndex, std::uint32_t serverOptimizationContract, bool serverContinuousFrames) : state_(std::make_shared<State>(std::move(loop), displayIndex, serverOptimizationContract, serverContinuousFrames)) {}
DesktopVideoRuntime::~DesktopVideoRuntime() { stop(); }
asio::awaitable<std::optional<DesktopVideoUpdate>> DesktopVideoRuntime::next(bool refresh) { return State::nextOwned(state_, refresh); }
asio::awaitable<bool> DesktopVideoRuntime::resize(std::uint32_t width, std::uint32_t height) { return State::resizeOwned(state_, width, height); }
void DesktopVideoRuntime::stop() {
    if (state_->stopped) {
        return;
    }
    state_->stopped = true;
    ++state_->worker->requestedRevision;
    state_->flow.retireRefinement(VideoClock::now());
    state_->pace.cancel();
    if (!state_->pending) {
        state_->releaseTimer();
    }
}
void DesktopVideoRuntime::invalidate() {
    state_->reset = true;
    ++state_->worker->requestedRevision;
    state_->flow.retireRefinement(VideoClock::now());
    state_->pace.cancel();
}
void DesktopVideoRuntime::observeRoundTrip(std::chrono::microseconds elapsed) {
    state_->flow.observeRoundTrip(elapsed, std::chrono::steady_clock::now());
}
DesktopVideoDiagnostics DesktopVideoRuntime::diagnostics() const {
    auto diagnostics = state_->diagnostics;
    diagnostics.ownerRevision = state_->worker->requestedRevision.load();
    return diagnostics;
}
void DesktopVideoRuntime::setOptimizationContract(std::uint32_t contract) {
    const auto permission = contract == 2 ? 2u : 0u;
    if (state_->pendingOptimizationContract == permission) {
        return;
    }
    state_->pendingOptimizationContract = permission;
    state_->qualityRefresh = true;
    state_->baselineRequired = true;
    ++state_->worker->requestedRevision;
    state_->flow.retireRefinement(VideoClock::now());
    state_->pace.cancel();
    if (!state_->requestsOptimization() && !state_->pending) {
        state_->releaseTimer();
    }
}
void DesktopVideoRuntime::capabilities(const wire::DesktopVideoCapabilities& capabilities) {
    const auto signature = [](const wire::DesktopVideoCapabilities& value) {
        unsigned mask = 1u << wire::AVC;
        for (const auto codec : value.codecs()) {
            mask |= 1u << codec;
        }
        return mask;
    };
    const bool gateChanged = (state_->capabilities.desktop_optimization_contract() == 2) != (capabilities.desktop_optimization_contract() == 2) && state_->serverOptimizationContract == 2;
    const bool regionPermissionChanged = state_->capabilities.region_updates() != capabilities.region_updates() || state_->capabilities.tile_cache() != capabilities.tile_cache() || state_->capabilities.continuous_frames() != capabilities.continuous_frames();
    if (gateChanged || regionPermissionChanged || signature(state_->capabilities) != signature(capabilities)) {
        state_->qualityRefresh = true;
        state_->baselineRequired = true;
        ++state_->worker->requestedRevision;
        state_->flow.retireRefinement(VideoClock::now());
    }
    state_->capabilities = capabilities;
    state_->pace.cancel();
    if (!state_->requestsOptimization() && !state_->pending && gateChanged) {
        state_->releaseTimer();
    }
}
void DesktopVideoRuntime::refinementAck(const wire::Envelope& acknowledgement) {
    if (!acknowledgement.has_desktop_refinement_ack() || acknowledgement.base_frame_id() != 0 || !acknowledgement.generation() || !acknowledgement.frame_id()) {
        return;
    }
    const auto& body = acknowledgement.desktop_refinement_ack();
    if (!body.batch_id() || (body.status() != wire::DesktopRefinementAck::APPLIED && body.status() != wire::DesktopRefinementAck::SUPERSEDED)) {
        return;
    }
    const auto now = VideoClock::now();
    if (!state_->flow.acknowledgeRefinement(acknowledgement.generation(), acknowledgement.frame_id(), body.batch_id(), now)) {
        return;
    }
    state_->refinementAcknowledgement = std::pair{DesktopDetailBatchKey{acknowledgement.generation(), acknowledgement.frame_id(), body.batch_id()},
                                                  body.status() == wire::DesktopRefinementAck::APPLIED ? DesktopDetailAck::kApplied : DesktopDetailAck::kSuperseded};
    state_->pace.cancel();
}
void DesktopVideoRuntime::feedback(const wire::QualityFeedback& feedback) {
    const bool wasBackground = state_->flow.background();
    const bool hadCredit = state_->flow.canEncode(std::chrono::steady_clock::now());
    if (!state_->flow.acknowledge(feedback.displayed_generation(), feedback.displayed_frame(),
                                  feedback.decode_microseconds(), feedback.queue_depth(), std::chrono::steady_clock::now(), feedback.background(),
                                  feedback.maximum_pending_video_frames())) {
        return;
    }
    if (wasBackground && !state_->flow.background()) {
        state_->qualityRefresh = true;
        state_->pace.cancel();
    }
    const bool detailChanged = state_->refinementEnabled != feedback.video_refinement();
    state_->refinementEnabled = feedback.video_refinement();
    if (detailChanged || feedback.background() != wasBackground) {
        ++state_->worker->requestedRevision;
        state_->flow.retireRefinement(VideoClock::now());
    }
    if (!wasBackground && state_->flow.background()) {
        state_->pace.cancel();
        if (!state_->pending) {
            state_->releaseTimer();
        }
    }
    if (!hadCredit && state_->flow.canEncode(std::chrono::steady_clock::now())) {
        state_->pace.cancel();
    }
    using Quality = DesktopFrameFlow::Quality;
    const bool wasQualityRefresh = state_->qualityRefresh;
    switch (feedback.preference()) {
    case wire::QualityFeedback::BALANCED:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kBalanced);
        break;
    case wire::QualityFeedback::SMOOTH:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kSmooth);
        break;
    case wire::QualityFeedback::SHARP:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kSharp);
        break;
    default:
        break;
    }
    if (!wasQualityRefresh && state_->qualityRefresh) {
        ++state_->worker->requestedRevision;
        state_->flow.retireRefinement(VideoClock::now());
    }
}
} // namespace ultraterimal::agent
