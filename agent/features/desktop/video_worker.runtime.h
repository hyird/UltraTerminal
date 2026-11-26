#pragma once
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
    bool keyRequired = false;
    bool repeated = false;
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
// ownership on the shared Ruvia worker/blocking pool.
// requestedRevision is the owner loop's cancellation/publication barrier.
struct DesktopVideoWorker {
    std::unique_ptr<DesktopCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::optional<CapturedFrame> retained;
    std::vector<std::uint8_t> convertedYuv;
    std::uint64_t convertedYuvEpoch = 0, convertedYuvSerial = 0;
    std::uint32_t convertedYuvWidth = 0, convertedYuvHeight = 0;
    VideoPixelFormat convertedYuvFormat = VideoPixelFormat::kI420;
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
    PreparedVideoStep prepare(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                              std::uint32_t flowWidth, std::uint32_t flowHeight, bool allowEncode,
                              const wire::DesktopVideoCapabilities& capabilities, bool requestedV2,
                              bool requestedContinuous, std::uint64_t revision, DesktopVideoStages stages,
                              std::chrono::steady_clock::time_point computeStarted);
    bool convertRowSlice(std::uint32_t rowBegin, std::uint32_t rowEnd, VideoPixelFormat format);
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
