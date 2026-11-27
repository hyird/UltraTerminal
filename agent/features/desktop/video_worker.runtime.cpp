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
    capture.reset();
    for (auto& buffer : yuvBuffers) {
        buffer.epoch = buffer.serial = 0;
        buffer.width = buffer.height = 0;
    }
    activeYuvSlot = 0;
    cachedDisplayModes.clear();
    cursor = DesktopCursor{};
    configurationPending = true;
    pixelsPending = false;
    lastPresentTicks = 0;
    coalescedCaptures = 0;
    consecutiveRepeatedFrames = 0;
    encodeAt = repeatAt = previousPixelCaptureAt = {};
}
void DesktopVideoWorker::beginGeneration() {
    if (generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Desktop generation exhausted");
    }
    ++generation;
    contentFrameId = deliveryFrameId = 0;
    consecutiveRepeatedFrames = 0;
    configurationPending = true;
}
bool DesktopVideoWorker::convertRowSlice(std::size_t yuvSlot, std::uint32_t rowBegin, std::uint32_t rowEnd, VideoPixelFormat format) {
    auto& buffer = yuvBuffers[yuvSlot];
    if (!retained || retained->bgra.empty() || buffer.bytes.size() < static_cast<std::size_t>(width) * height * 3 / 2) {
        return false;
    }
    return convertVideoPixelRows(retained->bgra, static_cast<std::size_t>(width) * 4, width, height, rowBegin, rowEnd, format, buffer.bytes);
}
PrefetchedCapture DesktopVideoWorker::prefetchCapture(std::size_t targetYuvSlot, std::uint32_t expectedWidth, std::uint32_t expectedHeight, std::uint64_t expectedEpoch) {
    PrefetchedCapture result;
    result.yuvSlot = targetYuvSlot;
    if (!capture) {
        return result;
    }
    auto& buffer = yuvBuffers[targetYuvSlot];
    DesktopCapture::DirectPixelConsumer directConsumer = [&](std::span<const std::uint8_t> bgra, std::size_t rowPitch,
                                                             std::uint32_t frameW, std::uint32_t frameH,
                                                             std::uint32_t rowBegin, std::uint32_t rowEnd,
                                                             std::uint64_t capEpoch, std::uint64_t serial, bool incremental) -> bool {
        if (frameW != expectedWidth || frameH != expectedHeight || capEpoch != expectedEpoch) {
            return false;
        }
        const auto yuvBytes = static_cast<std::size_t>(frameW) * frameH * 3 / 2;
        const bool hadStorage = buffer.bytes.size() >= yuvBytes;
        buffer.bytes.resize(yuvBytes);
        const bool cacheContinuous = hadStorage && incremental && buffer.epoch == capEpoch &&
                                     buffer.width == frameW && buffer.height == frameH &&
                                     buffer.format == VideoPixelFormat::kI420 &&
                                     buffer.serial + 1 == serial;
        const std::uint32_t convBegin = cacheContinuous ? (rowBegin & ~1u) : 0u;
        const std::uint32_t convEnd = cacheContinuous ? std::min(frameH, (rowEnd + 1u) & ~1u) : frameH;
        if (!convertVideoPixelRows(bgra, rowPitch, frameW, frameH, convBegin, convEnd, VideoPixelFormat::kI420, buffer.bytes)) {
            buffer.epoch = buffer.serial = 0;
            return false;
        }
        buffer.epoch = capEpoch;
        buffer.serial = serial;
        buffer.width = frameW;
        buffer.height = frameH;
        buffer.format = VideoPixelFormat::kI420;
        return true;
    };
    try {
        result.frame = capture->nextFrame(std::chrono::milliseconds(16), nullptr, &result.timing, true, false, &directConsumer);
    } catch (const DesktopChanged& error) {
        result.desktopChanged = true;
        result.faultCode = error.code();
    }
    return result;
}
PreparedVideoStep DesktopVideoWorker::encodeDeferred(DeferredCpuEncode deferred, bool preconverted) {
    auto& buffer = yuvBuffers[deferred.yuvSlot];
    const auto convertStarted = VideoClock::now();
    if (!preconverted) {
        preconverted = convertRowSlice(deferred.yuvSlot, deferred.rowBegin, deferred.rowEnd, deferred.pixelFormat);
        if (preconverted && deferred.rowBegin < deferred.rowEnd) {
            addDuration(deferred.stages.prepareCpu, DesktopVideoStages::microseconds(convertStarted, VideoClock::now()));
        }
    }
    if (preconverted) {
        buffer.epoch = deferred.sourceEpoch;
        buffer.serial = deferred.sourceSerial;
        buffer.width = deferred.width;
        buffer.height = deferred.height;
        buffer.format = deferred.pixelFormat;
    } else {
        buffer.epoch = buffer.serial = 0;
        buffer.width = buffer.height = 0;
    }
    const auto encodeStarted = VideoClock::now();
    encoder->setRate(deferred.rate.framesPerSecond, deferred.rate.bitrate);
    ++diagnostics.videoEncodeCalls;
    auto encoded = preconverted ? encoder->encodePreconverted(retained->bgra, static_cast<std::size_t>(width) * 4, buffer.bytes, deferred.pixelFormat, deferred.timestamp, deferred.keyRequired)
                                : encoder->encode(retained->bgra, static_cast<std::size_t>(width) * 4, deferred.timestamp, deferred.keyRequired);
    auto finishStep = [&](std::optional<DesktopVideoUpdate> update) {
        const auto completedAt = VideoClock::now();
        if (update) {
            update->workerDispatchMicroseconds = DesktopVideoStages::microseconds(deferred.computeStarted, completedAt);
        }
        const auto wakeAt = encodeAt > completedAt ? encodeAt : completedAt;
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
    update.changedPixels = deferred.repeated ? 0 : deferred.changedPixels;
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
        for (const auto& mode : cachedDisplayModes) {
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
    update.productionMicroseconds = std::max(update.frame.encode_microseconds(), deferred.stages.productionMicroseconds());
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
                                              std::chrono::steady_clock::time_point computeStarted,
                                              std::optional<PrefetchedCapture> prefetched) {
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
            prefetched.reset();
        }
        if (!recovery.ready(started)) {
            return finishImmediate(std::nullopt);
        }
        if (!capture) {
            capture = std::make_unique<DesktopCapture>(display);
            cachedDisplayModes = capture->displayModes();
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
        if (prefetched) {
            if (prefetched->desktopChanged) {
                throw DesktopChanged("Desktop capture invalidated during prefetch", prefetched->faultCode);
            }
            timing = prefetched->timing;
            pixels = std::move(prefetched->frame);
            if (pixels) {
                const auto& prefetchedYuv = yuvBuffers[prefetched->yuvSlot];
                if (prefetchedYuv.epoch == pixels->captureEpoch && prefetchedYuv.serial == pixels->sourceSerial &&
                    prefetchedYuv.width == pixels->width && prefetchedYuv.height == pixels->height) {
                    activeYuvSlot = prefetched->yuvSlot;
                }
            }
        } else {
            // Acquire while backpressured, retaining only the latest raw snapshot.
            // CPU readback starts only when a frame can actually be encoded.
            const bool readPixels = allowEncode && (!encoder || !encoder->gpu()) &&
                                    (refresh || configurationPending || started >= encodeAt);
            const auto preferredFormat = (encoder && !encoder->gpu()) ? encoder->preferredPixelFormat() : std::nullopt;
            const bool canDirectI420 = readPixels && preferredFormat && *preferredFormat == VideoPixelFormat::kI420 && !configurationChanged;
            auto& activeBuffer = yuvBuffers[activeYuvSlot];
            DesktopCapture::DirectPixelConsumer directConsumer;
            if (canDirectI420) {
                directConsumer = [&](std::span<const std::uint8_t> bgra, std::size_t rowPitch,
                                     std::uint32_t frameW, std::uint32_t frameH,
                                     std::uint32_t rowBegin, std::uint32_t rowEnd,
                                     std::uint64_t capEpoch, std::uint64_t serial, bool incremental) -> bool {
                    if (frameW != width || frameH != height || capEpoch != configuredCaptureEpoch) {
                        return false;
                    }
                    const auto yuvBytes = static_cast<std::size_t>(frameW) * frameH * 3 / 2;
                    const bool hadStorage = activeBuffer.bytes.size() >= yuvBytes;
                    activeBuffer.bytes.resize(yuvBytes);
                    const bool cacheContinuous = hadStorage && incremental && activeBuffer.epoch == capEpoch &&
                                                 activeBuffer.width == frameW && activeBuffer.height == frameH &&
                                                 activeBuffer.format == VideoPixelFormat::kI420 &&
                                                 activeBuffer.serial + 1 == serial;
                    const std::uint32_t convBegin = cacheContinuous ? (rowBegin & ~1u) : 0u;
                    const std::uint32_t convEnd = cacheContinuous ? std::min(frameH, (rowEnd + 1u) & ~1u) : frameH;
                    if (!convertVideoPixelRows(bgra, rowPitch, frameW, frameH, convBegin, convEnd, VideoPixelFormat::kI420, activeBuffer.bytes)) {
                        activeBuffer.epoch = activeBuffer.serial = 0;
                        return false;
                    }
                    activeBuffer.epoch = capEpoch;
                    activeBuffer.serial = serial;
                    activeBuffer.width = frameW;
                    activeBuffer.height = frameH;
                    activeBuffer.format = VideoPixelFormat::kI420;
                    return true;
                };
            }
            const bool retainTexture = !encoder || encoder->gpu() || !readPixels;
            const auto wait = allowEncode && !pixelsPending ? std::chrono::milliseconds(16) : std::chrono::milliseconds(0);
            pixels = capture->nextFrame(wait, retained ? &retained->bgra : nullptr, &timing, readPixels, retainTexture, canDirectI420 ? &directConsumer : nullptr);
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
            cachedDisplayModes = capture->displayModes();
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
        const auto directFormat = !gpuEncoding ? encoder->preferredPixelFormat() : std::nullopt;
        auto& activeBuffer = yuvBuffers[activeYuvSlot];
        const bool alreadyConvertedDirect = directFormat && *directFormat == VideoPixelFormat::kI420 &&
                                            activeBuffer.bytes.size() >= static_cast<std::size_t>(width) * height * 3 / 2 &&
                                            activeBuffer.epoch == source.captureEpoch && activeBuffer.serial == source.sourceSerial &&
                                            activeBuffer.width == width && activeBuffer.height == height &&
                                            activeBuffer.format == VideoPixelFormat::kI420;
        const auto materialize = [&] {
            if (source.bgra.empty()) {
                CaptureTiming readbackTiming;
                capture->readTexture(source, &readbackTiming);
                addDuration(stages.gpuSyncWait, readbackTiming.stages.mapMicroseconds);
                addDuration(stages.prepareCpu, readbackTiming.stages.rowCopyMicroseconds);
            }
        };
        if (!gpuEncoding && !alreadyConvertedDirect) {
            materialize();
        } else if (alreadyConvertedDirect && !source.bgra.empty()) {
            std::vector<std::uint8_t>().swap(source.bgra);
        }
        bool repeated = continuousFrames && contentFrameId && !pixelsPending && !refresh && !configurationPending;
        const auto slots = nextDesktopFrameSlots({encodeAt, repeatAt}, started, rate.framesPerSecond, continuousFrames, refresh || configurationPending, repeated);
        encodeAt = slots.content;
        if (repeated) {
            ++consecutiveRepeatedFrames;
            // After 3 repeated frames settle static residuals, drop idle repeat
            // cadence to 5 FPS to avoid wasting CPU and receiver credit.
            repeatAt = consecutiveRepeatedFrames >= 3 ? started + std::chrono::milliseconds(200) : slots.repeated;
        } else {
            consecutiveRepeatedFrames = 0;
            repeatAt = slots.repeated;
        }
        const auto encodeStarted = VideoClock::now();
        const auto timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(encodeStarted - epoch).count());
        const bool keyRequired = refresh || configurationPending;
        const auto changedArea = source.changedPixels ? source.changedPixels : static_cast<std::uint64_t>(width) * height;
        if (const auto pixelFormat = encoder->preferredPixelFormat(); pixelFormat && !gpuEncoding) {
            const auto yuvBytes = static_cast<std::size_t>(width) * height * 3 / 2;
            const bool hadStorage = activeBuffer.bytes.size() >= yuvBytes;
            activeBuffer.bytes.resize(yuvBytes);
            const bool sameCachedYuv = hadStorage && activeBuffer.epoch == source.captureEpoch && activeBuffer.width == width &&
                                       activeBuffer.height == height && activeBuffer.format == *pixelFormat;
            std::uint32_t rowBegin = 0;
            std::uint32_t rowEnd = height;
            if (sameCachedYuv && activeBuffer.serial == source.sourceSerial) {
                rowBegin = 0;
                rowEnd = 0;
            } else if (sameCachedYuv && activeBuffer.serial + 1 == source.sourceSerial &&
                       source.dirtyRowBegin <= source.dirtyRowEnd && source.dirtyRowEnd <= height) {
                rowBegin = source.dirtyRowBegin & ~1u;
                rowEnd = std::min(height, (source.dirtyRowEnd + 1u) & ~1u);
            }
            const bool canPrefetchNext = !configurationPending && !keyRequired && !repeated &&
                                         *pixelFormat == VideoPixelFormat::kI420 && !DesktopCursor::requiresBackground(source);
            DeferredCpuEncode deferred{std::move(update), stages, rate, *pixelFormat, width, height, rowBegin, rowEnd, source.captureEpoch, source.sourceSerial, changedArea, computeStarted, timestamp, activeYuvSlot, keyRequired, repeated, canPrefetchNext};
            const auto convertHeight = rowEnd > rowBegin ? rowEnd - rowBegin : 0u;
            const auto dirtyPixels = static_cast<std::uint64_t>(width) * convertHeight;
            if (!canPrefetchNext && (convertHeight == 0 || dirtyPixels < 640ull * 360)) {
                return encodeDeferred(std::move(deferred), convertHeight == 0);
            }
            PreparedVideoStep step;
            step.deferredEncode = std::move(deferred);
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
        update.changedPixels = repeated ? 0 : changedArea;
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
        update.productionMicroseconds = std::max(update.frame.encode_microseconds(), stages.productionMicroseconds());
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
        const bool preconverted = step.deferredEncode->rowBegin == step.deferredEncode->rowEnd;
        step = encodeDeferred(std::move(*step.deferredEncode), preconverted);
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
