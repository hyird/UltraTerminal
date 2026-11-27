#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "frame_flow.runtime.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/cursor.transport.h"
#include "recovery.runtime.h"
#include "ultraterimal/protocol/desktop_session.protocol.h"
#include "video.transport.h"
#include "video.types.h"
#include "video_color.protocol.h"

namespace ultraterimal::agent {
struct YuvFrameBuffer {
    std::vector<std::uint8_t> bytes;
    std::uint64_t epoch = 0, serial = 0;
    std::uint32_t width = 0, height = 0;
    VideoPixelFormat format = VideoPixelFormat::kI420;
};
struct PrefetchedCapture {
    std::optional<CapturedFrame> frame;
    CaptureTiming timing;
    std::size_t yuvSlot = 0;
    bool desktopChanged = false;
    wire::DesktopFault::Code faultCode = wire::DesktopFault::CODE_UNKNOWN;
};
struct DeferredCpuEncode {
    DesktopVideoUpdate update;
    DesktopVideoStages stages;
    DesktopFrameFlow::Rate rate;
    VideoPixelFormat pixelFormat = VideoPixelFormat::kI420;
    std::uint32_t width = 0, height = 0;
    std::uint32_t rowBegin = 0, rowEnd = 0;
    std::uint64_t sourceEpoch = 0, sourceSerial = 0, changedPixels = 0;
    std::chrono::steady_clock::time_point computeStarted{};
    std::uint64_t timestamp = 0;
    std::size_t yuvSlot = 0;
    bool keyRequired = false;
    bool repeated = false;
    bool canPrefetchNext = false;
};
struct PreparedVideoStep {
    std::optional<DesktopVideoUpdate> immediate;
    std::optional<DeferredCpuEncode> deferredEncode;
    std::chrono::steady_clock::time_point retryAt{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged;
    DesktopVideoDiagnostics diagnostics;
    bool baselineRequired = true;
    bool activeV2 = false;
};
// Capture, pixels and encoder are accessed serially through single-flight coroutine
// ownership on the shared Ruvia worker/blocking pool, except when encodeDeferred()
// on yuvBuffers[slot] overlaps with prefetchCapture() on yuvBuffers[1 - slot],
// which touch strictly disjoint state (encoder vs capture, distinct YUV buffers).
// requestedRevision is the owner loop's cancellation/publication barrier.
struct DesktopVideoWorker {
    std::unique_ptr<DesktopCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::optional<CapturedFrame> retained;
    std::array<YuvFrameBuffer, 2> yuvBuffers;
    std::size_t activeYuvSlot = 0;
    std::vector<DesktopCapture::DisplayMode> cachedDisplayModes;
    DesktopCursor cursor;
    CaptureRecovery recovery;
    DesktopVideoDiagnostics diagnostics;
    std::atomic<std::uint64_t> requestedRevision{0};
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point encodeAt{}, repeatAt{}, previousPixelCaptureAt{};
    std::uint64_t generation = 0, contentFrameId = 0, deliveryFrameId = 0;
    std::uint64_t configuredCaptureEpoch = 0, lastPresentTicks = 0;
    std::uint32_t width = 0, height = 0, coalescedCaptures = 0, consecutiveRepeatedFrames = 0;
    std::int32_t configuredLeft = 0, configuredTop = 0;
    bool configurationPending = true, pixelsPending = false;
    bool optimizationV2 = false, continuousFrames = false;
    std::vector<wire::VideoCodec> requestedCodecs{wire::AVC};
    protocol::DesktopSessionAgreement sessionAgreement;

    void discard();
    bool resize(std::uint32_t requestedWidth, std::uint32_t requestedHeight);
    PrefetchedCapture prefetchCapture(std::size_t targetYuvSlot, std::uint32_t expectedWidth, std::uint32_t expectedHeight, std::uint64_t expectedEpoch);
    PreparedVideoStep prepare(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                              std::uint32_t flowWidth, std::uint32_t flowHeight, bool allowEncode,
                              const wire::DesktopVideoCapabilities& capabilities, bool requestedV2,
                              bool requestedContinuous, std::uint64_t revision, DesktopVideoStages stages,
                              std::chrono::steady_clock::time_point computeStarted,
                              std::optional<PrefetchedCapture> prefetched = std::nullopt);
    bool convertRowSlice(std::size_t yuvSlot, std::uint32_t rowBegin, std::uint32_t rowEnd, VideoPixelFormat format);
    PreparedVideoStep encodeDeferred(DeferredCpuEncode deferred, bool preconverted);
    std::optional<DesktopVideoUpdate> next(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                                           std::uint32_t flowWidth, std::uint32_t flowHeight,
                                           std::optional<std::pair<std::uint32_t, std::uint32_t>>& geometryChanged,
                                           bool allowEncode, const wire::DesktopVideoCapabilities& capabilities,
                                           bool requestedV2, bool requestedContinuous, std::uint64_t revision,
                                           DesktopVideoStages stages);

  private:
    void beginGeneration();
};
} // namespace ultraterimal::agent
