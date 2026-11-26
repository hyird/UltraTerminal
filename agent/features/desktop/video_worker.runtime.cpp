#include "video_worker.runtime.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include <windows.h>

#include "frame_cadence.runtime.h"

namespace ultraterimal::agent {
namespace {
using VideoClock = std::chrono::steady_clock;

void addDuration(std::optional<std::uint32_t>& total, std::optional<std::uint32_t> sample) {
    if (sample) {
        total = std::min(60000000u, total.value_or(0) + *sample);
    }
}
void writeStages(wire::DesktopStageTiming& target, const DesktopVideoStages& stages) {
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
    if (stages.prepareCpu) {
        target.set_prepare_cpu_microseconds(*stages.prepareCpu);
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
// All capture and codec resources belong to the bounded encoding worker.

} // namespace
void DesktopVideoWorker::discard() {
    encoder.reset();
    retained.reset();
    prefetched.reset();
    capture.reset();
    ++captureInstance;
    cursor = DesktopCursor{};
    configurationPending = true;
    pixelsPending = false;
    lastPresentTicks = 0;
    coalescedCaptures = 0;
    encodeAt = repeatAt = previousPixelCaptureAt = {};
}
void DesktopVideoWorker::beginGeneration() {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Desktop generation exhausted");
    }
    ++generation;
    contentFrameId = deliveryFrameId = 0;
    configurationPending = true;
}
PrefetchedCapture DesktopVideoWorker::prefetchNext(std::uint64_t instance) {
    PrefetchedCapture out;
    out.captureInstance = instance;
    if (!capture || instance != captureInstance) {
        return out;
    }
    try {
        out.frame = capture->nextFrame(std::chrono::milliseconds(16), &prefetchScratch, &out.timing, true, false);
    } catch (const DesktopChanged& error) {
        out.resetCode = error.code();
    } catch (...) {
        out.failure = std::current_exception();
    }
    return out;
}
bool DesktopVideoWorker::convertRowSlice(std::uint32_t rowBegin, std::uint32_t rowEnd, VideoPixelFormat format) {
    if (!retained || retained->bgra.empty() || convertedYuv.size() < static_cast<std::size_t>(width) * height * 3 / 2) {
        return false;
    }
    return convertVideoPixelRows(retained->bgra, static_cast<std::size_t>(width) * 4, width, height, rowBegin, rowEnd, format, convertedYuv);
}
PreparedVideoStep DesktopVideoWorker::encodeDeferred(DeferredCpuEncode deferred, bool preconverted) {
    const auto encodeStarted = VideoClock::now();
    encoder->setRate(deferred.rate.framesPerSecond, deferred.rate.bitrate);
    ++diagnostics.videoEncodeCalls;
    auto encoded = preconverted ? encoder->encodePreconverted(retained->bgra, static_cast<std::size_t>(width) * 4, convertedYuv, deferred.pixelFormat, deferred.timestamp, deferred.keyRequired)
                                : encoder->encode(retained->bgra, static_cast<std::size_t>(width) * 4, deferred.timestamp, deferred.keyRequired);
    auto finishStep = [&](std::optional<DesktopVideoUpdate> update) {
        const auto completedAt = VideoClock::now();
        if (update) {
            update->workerDispatchMicroseconds = DesktopVideoStages::microseconds(deferred.computeStarted, completedAt);
        }
        const auto wakeAt = pixelsPending && encodeAt > completedAt ? encodeAt : completedAt + std::chrono::milliseconds(1);
        PreparedVideoStep step;
        step.immediate = std::move(update);
        step.retryAt = std::max(recovery.nextAttempt(), wakeAt);
        step.diagnostics = diagnostics;
        step.baselineRequired = !encoder;
        step.activeV2 = optimizationV2;
        return step;
    };
    if (!encoded) {
        if (encoded.error() == VideoEncodeError::kFrameSkipped) {
            return finishStep(deferred.update.cursor ? std::optional(std::move(deferred.update)) : std::nullopt);
        }
        throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop video encoding failed");
    }
    if ((deferred.keyRequired || encoded->baselineReset) && !encoded->keyFrame) {
        throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop refresh did not produce a key frame");
    }
    if (encoded->baselineReset) {
        beginGeneration();
        deferred.repeated = false;
        deferred.update.cursor = cursor.update(*retained, true);
    }
    if (deliveryFrameId == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Desktop frame sequence exhausted");
    }
    auto& update = deferred.update;
    update.generation = generation;
    update.frameId = ++deliveryFrameId;
    update.baseFrameId = deferred.repeated ? contentFrameId : 0;
    update.changedPixels = deferred.repeated ? 0 : static_cast<std::uint64_t>(width) * height;
    if (!deferred.repeated) {
        contentFrameId = deliveryFrameId;
    }
    pixelsPending = false;
    if (configurationPending) {
        auto& configuration = update.configuration.emplace();
        configuration.set_codec(encoded->codec);
        configuration.set_width(width);
        configuration.set_height(height);
        configuration.set_desktop_optimization_contract(2);
        configuration.set_continuous_frames(continuousFrames);
        *configuration.mutable_session_capabilities() = protocol::desktopSessionCapabilities();
        configuration.set_session_version(sessionAgreement.version);
        configuration.set_session_features(sessionAgreement.features);
        for (const auto& mode : capture->displayModes()) {
            auto* advertised = configuration.add_display_modes();
            advertised->set_width(mode.width);
            advertised->set_height(mode.height);
        }
        configurationPending = false;
    }
    update.frame.set_data(encoded->annexB.data(), encoded->annexB.size());
    update.frame.set_key_frame(encoded->keyFrame);
    update.frame.set_repeated(deferred.repeated);
    update.frame.set_timestamp_microseconds(encoded->timestampMicroseconds);
    update.frame.set_encode_microseconds(std::max(1u, DesktopVideoStages::microseconds(encodeStarted, VideoClock::now())));
    update.frame.set_hardware_encoding(encoder->hardware());
    update.frame.set_gpu_encoding(false);
    update.frame.set_target_frames_per_second(deferred.rate.framesPerSecond);
    update.frame.set_target_bitrate(deferred.rate.bitrate);
    deferred.stages.sourceAge = sourceAge(lastPresentTicks);
    deferred.stages.videoEncode = update.frame.encode_microseconds();
    deferred.stages.coalescedCaptures = std::exchange(coalescedCaptures, 0);
    if (optimizationV2) {
        writeStages(*update.frame.mutable_stage_timing(), deferred.stages);
    }
    ++diagnostics.encodedFrames;
    return finishStep(std::move(update));
}
PreparedVideoStep DesktopVideoWorker::prepare(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                                              std::uint32_t flowWidth, std::uint32_t flowHeight, bool allowEncode,
                                              const wire::DesktopVideoCapabilities& capabilities, bool requestedV2,
                                              bool requestedContinuous, std::uint64_t revision, DesktopVideoStages stages,
                                              std::chrono::steady_clock::time_point computeStarted) {
    auto finishImmediate = [&](std::optional<DesktopVideoUpdate> update, std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged = std::nullopt) {
        const auto completedAt = VideoClock::now();
        if (update) {
            update->workerDispatchMicroseconds = DesktopVideoStages::microseconds(computeStarted, completedAt);
        }
        const auto wakeAt = allowEncode && pixelsPending && encodeAt > completedAt
                                ? encodeAt
                                : completedAt + std::chrono::milliseconds(allowEncode ? 1 : 33);
        PreparedVideoStep step;
        step.immediate = std::move(update);
        step.retryAt = std::max(recovery.nextAttempt(), wakeAt);
        step.geometryChanged = geometryChanged;
        step.diagnostics = diagnostics;
        step.baselineRequired = !encoder;
        step.activeV2 = optimizationV2;
        return step;
    };
    try {
        const auto started = VideoClock::now();
        diagnostics.lastJobRevision = revision;
        diagnostics.lastJobAcquired = false;
        if (reset) {
            discard();
        }
        if (!recovery.ready(started)) {
            return finishImmediate(std::nullopt);
        }
        if (!capture) {
            capture = std::make_unique<DesktopCapture>(display);
            ++captureInstance;
        }
        std::vector<wire::VideoCodec> codecs{wire::AVC};
        for (const auto codec : capabilities.codecs()) {
            if (codec != wire::AVC) {
                codecs.push_back(static_cast<wire::VideoCodec>(codec));
            }
        }
        const auto agreement = protocol::negotiateDesktopSession(capabilities.session_capabilities());
        const bool configurationChanged = codecs != requestedCodecs || requestedV2 != optimizationV2 ||
                                          requestedContinuous != continuousFrames || agreement != sessionAgreement;
        refresh = refresh || configurationChanged;
        CaptureTiming timing;
        std::optional<CapturedFrame> pixels;
        if (prefetched && prefetched->captureInstance == captureInstance) {
            auto item = std::move(*prefetched);
            prefetched.reset();
            if (item.failure) {
                std::rethrow_exception(item.failure);
            }
            if (item.resetCode) {
                throw CaptureReset(*item.resetCode);
            }
            timing = item.timing;
            pixels = std::move(item.frame);
        } else {
            prefetched.reset();
            // Acquire while backpressured, retaining only the latest raw snapshot.
            // CPU readback starts only when a frame can actually be encoded.
            const bool readPixels = allowEncode && (!encoder || !encoder->gpu()) &&
                                    (refresh || configurationPending || started >= encodeAt);
            const bool retainTexture = !encoder || encoder->gpu() || !readPixels || !retained || retained->bgra.empty();
            const auto wait = allowEncode && !pixelsPending ? std::chrono::milliseconds(16) : std::chrono::milliseconds(0);
            pixels = capture->nextFrame(wait, retained ? &retained->bgra : nullptr, &timing, readPixels, retainTexture);
        }
        ++diagnostics.captureCalls;
        addDuration(stages.acquireWait, timing.stages.acquireWaitMicroseconds);
        addDuration(stages.gpuSyncWait, timing.stages.mapMicroseconds);
        addDuration(stages.prepareCpu, timing.stages.rowCopyMicroseconds);
        if (pixels) {
            diagnostics.lastJobAcquired = true;
            ++diagnostics.returnedSnapshots;
            diagnostics.lastAcquireEpoch = pixels->captureEpoch;
            diagnostics.lastAcquireSerial = pixels->sourceSerial;
            diagnostics.lastAcquirePresentTicks = pixels->captureTicks;
            const bool changed = !retained || pixels->sourceSerial != retained->sourceSerial || pixels->captureEpoch != retained->captureEpoch;
            if (changed) {
                ++diagnostics.pixelUpdates;
                coalescedCaptures += pixelsPending;
                pixelsPending = true;
                if (previousPixelCaptureAt != VideoClock::time_point{}) {
                    ++diagnostics.pixelCaptureGaps;
                    diagnostics.pixelCaptureGapsGe45ms += started - previousPixelCaptureAt >= std::chrono::milliseconds(45);
                }
                previousPixelCaptureAt = started;
                lastPresentTicks = pixels->captureTicks;
            } else {
                ++diagnostics.cursorOnlyUpdates;
            }
            if (!changed && pixels->bgra.empty() && retained) {
                pixels->bgra = std::move(retained->bgra);
            } else if (retained && !retained->bgra.empty() && !pixels->bgra.empty()) {
                prefetchScratch = std::move(retained->bgra);
            }
            retained = std::move(pixels);
        } else {
            ++diagnostics.captureTimeouts;
        }
        if (!retained) {
            return finishImmediate(std::nullopt);
        }
        auto& source = *retained;
        if (source.rotation != 1 || source.width % 2 || source.height % 2) {
            throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop orientation or dimensions require capture reconfiguration");
        }
        if (source.width != flowWidth || source.height != flowHeight) {
            return finishImmediate(std::nullopt, std::pair{source.width, source.height});
        }
        // XOR/monochrome cursor pixels depend on the same complete snapshot.
        if (DesktopCursor::requiresBackground(source) && source.bgra.empty()) {
            CaptureTiming readbackTiming;
            capture->readTexture(source, &readbackTiming);
            addDuration(stages.gpuSyncWait, readbackTiming.stages.mapMicroseconds);
            addDuration(stages.prepareCpu, readbackTiming.stages.rowCopyMicroseconds);
        }
        DesktopVideoUpdate update;
        update.desktopLeft = source.desktopLeft;
        update.desktopTop = source.desktopTop;
        update.sourcePresentTicks = lastPresentTicks;
        update.cursor = cursor.update(source, refresh || configurationPending);
        if (!allowEncode) {
            ++diagnostics.backpressuredCalls;
            return finishImmediate(update.cursor ? std::optional(std::move(update)) : std::nullopt);
        }
        const auto now = VideoClock::now();
        const bool repeatDue = continuousFrames && contentFrameId && now >= repeatAt;
        if ((!pixelsPending && !refresh && !configurationPending && !repeatDue) ||
            (!refresh && !configurationPending && now < encodeAt)) {
            ++diagnostics.pendingBeforeDeadlineCalls;
            return finishImmediate(update.cursor ? std::optional(std::move(update)) : std::nullopt);
        }
        const bool sourceChanged = encoder && (source.width != width || source.height != height ||
                                               source.captureEpoch != configuredCaptureEpoch || source.desktopLeft != configuredLeft || source.desktopTop != configuredTop);
        if (!encoder || sourceChanged || configurationChanged) {
            encoder = std::make_unique<VideoEncoder>(VideoEncoderConfig{source.width, source.height, rate.bitrate, rate.framesPerSecond, false, codecs}, capture->device());
            width = source.width;
            height = source.height;
            configuredCaptureEpoch = source.captureEpoch;
            configuredLeft = source.desktopLeft;
            configuredTop = source.desktopTop;
            requestedCodecs = std::move(codecs);
            optimizationV2 = requestedV2;
            continuousFrames = requestedContinuous;
            sessionAgreement = agreement;
            beginGeneration();
        }
        const bool gpuEncoding = encoder->gpu();
        const auto materialize = [&] {
            if (source.bgra.empty()) {
                CaptureTiming readbackTiming;
                capture->readTexture(source, &readbackTiming);
                addDuration(stages.gpuSyncWait, readbackTiming.stages.mapMicroseconds);
                addDuration(stages.prepareCpu, readbackTiming.stages.rowCopyMicroseconds);
            }
        };
        if (!gpuEncoding) {
            materialize();
        }
        bool repeated = continuousFrames && contentFrameId && !pixelsPending && !refresh && !configurationPending;
        const auto slots = nextDesktopFrameSlots({encodeAt, repeatAt}, started, rate.framesPerSecond, continuousFrames, refresh || configurationPending, repeated);
        encodeAt = slots.content;
        repeatAt = slots.repeated;
        const auto encodeStarted = VideoClock::now();
        const auto timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(encodeStarted - epoch).count());
        const bool keyRequired = refresh || configurationPending;
        if (const auto pixelFormat = encoder->preferredPixelFormat(); pixelFormat && !gpuEncoding) {
            convertedYuv.resize(static_cast<std::size_t>(width) * height * 3 / 2);
            PreparedVideoStep step;
            step.deferredEncode = DeferredCpuEncode{std::move(update), stages, rate, *pixelFormat, width, height, computeStarted, timestamp, (!encoder->hardware() && !configurationPending && !refresh) ? captureInstance : 0, keyRequired, repeated};
            step.diagnostics = diagnostics;
            step.baselineRequired = false;
            step.activeV2 = optimizationV2;
            return step;
        }
        encoder->setRate(rate.framesPerSecond, rate.bitrate);
        ++diagnostics.videoEncodeCalls;
        auto encoded = gpuEncoding ? encoder->encodeTexture(capture->texture(), timestamp, keyRequired)
                                   : encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, timestamp, keyRequired);
        if (!encoded && encoded.error() == VideoEncodeError::kGpuUnavailable) {
            materialize();
            ++diagnostics.videoEncodeCalls;
            encoded = encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, timestamp, true);
        }
        if (!encoded) {
            if (encoded.error() == VideoEncodeError::kFrameSkipped) {
                return finishImmediate(update.cursor ? std::optional(std::move(update)) : std::nullopt);
            }
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop video encoding failed");
        }
        if ((keyRequired || encoded->baselineReset) && !encoded->keyFrame) {
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop refresh did not produce a key frame");
        }
        if (encoded->baselineReset) {
            beginGeneration();
            repeated = false;
            update.cursor = cursor.update(source, true);
        }
        if (deliveryFrameId == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop frame sequence exhausted");
        }
        update.generation = generation;
        update.frameId = ++deliveryFrameId;
        update.baseFrameId = repeated ? contentFrameId : 0;
        update.changedPixels = repeated ? 0 : static_cast<std::uint64_t>(width) * height;
        if (!repeated) {
            contentFrameId = deliveryFrameId;
        }
        pixelsPending = false;
        if (configurationPending) {
            auto& configuration = update.configuration.emplace();
            configuration.set_codec(encoded->codec);
            configuration.set_width(width);
            configuration.set_height(height);
            configuration.set_desktop_optimization_contract(2);
            configuration.set_continuous_frames(continuousFrames);
            *configuration.mutable_session_capabilities() = protocol::desktopSessionCapabilities();
            configuration.set_session_version(sessionAgreement.version);
            configuration.set_session_features(sessionAgreement.features);
            for (const auto& mode : capture->displayModes()) {
                auto* advertised = configuration.add_display_modes();
                advertised->set_width(mode.width);
                advertised->set_height(mode.height);
            }
            configurationPending = false;
        }
        update.frame.set_data(encoded->annexB.data(), encoded->annexB.size());
        update.frame.set_key_frame(encoded->keyFrame);
        update.frame.set_repeated(repeated);
        update.frame.set_timestamp_microseconds(encoded->timestampMicroseconds);
        update.frame.set_encode_microseconds(std::max(1u, DesktopVideoStages::microseconds(encodeStarted, VideoClock::now())));
        update.frame.set_hardware_encoding(encoder->hardware());
        update.frame.set_gpu_encoding(gpuEncoding && encoder->gpu());
        update.frame.set_target_frames_per_second(rate.framesPerSecond);
        update.frame.set_target_bitrate(rate.bitrate);
        stages.sourceAge = sourceAge(lastPresentTicks);
        stages.videoEncode = update.frame.encode_microseconds();
        stages.coalescedCaptures = std::exchange(coalescedCaptures, 0);
        if (optimizationV2) {
            writeStages(*update.frame.mutable_stage_timing(), stages);
        }
        ++diagnostics.encodedFrames;
        return finishImmediate(std::move(update));
    } catch (const DesktopChanged& error) {
        discard();
        if (!recovery.restart(VideoClock::now())) {
            throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
        }
        return finishImmediate(std::nullopt);
    }
}
std::optional<DesktopVideoUpdate> DesktopVideoWorker::next(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                                                           std::uint32_t flowWidth, std::uint32_t flowHeight,
                                                           std::optional<std::pair<std::uint32_t, std::uint32_t>>& geometryChanged,
                                                           bool allowEncode, const wire::DesktopVideoCapabilities& capabilities,
                                                           bool requestedV2, bool requestedContinuous, std::uint64_t revision,
                                                           DesktopVideoStages stages) {
    auto step = prepare(display, refresh, reset, rate, flowWidth, flowHeight, allowEncode, capabilities, requestedV2, requestedContinuous, revision, stages, VideoClock::now());
    if (step.deferredEncode) {
        step = encodeDeferred(std::move(*step.deferredEncode), false);
    }
    geometryChanged = step.geometryChanged;
    return std::move(step.immediate);
}
bool DesktopVideoWorker::resize(std::uint32_t requestedWidth, std::uint32_t requestedHeight) {
    try {
        if (!capture || !capture->resize(requestedWidth, requestedHeight)) {
            return false;
        }
        discard();
        // Explicit mode changes begin a new capture recovery episode.
        recovery = CaptureRecovery{};
        return true;
    } catch (const DesktopChanged&) {
        discard();
        return false;
    }
}

} // namespace ultraterimal::agent
