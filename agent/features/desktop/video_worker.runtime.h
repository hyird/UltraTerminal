#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
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

namespace ultraterimal::agent {
// Capture, encoder and snapshots are touched only by the bounded worker.
// requestedRevision is the owner loop's cancellation/publication barrier.
struct DesktopVideoWorker {
    std::unique_ptr<DesktopCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::optional<CapturedFrame> retained;
    DesktopCursor cursor;
    CaptureRecovery recovery;
    DesktopVideoDiagnostics diagnostics;
    std::atomic<std::uint64_t> requestedRevision{0};
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point encodeAt{}, repeatAt{}, previousPixelCaptureAt{};
    std::uint64_t generation = 0, contentFrameId = 0, deliveryFrameId = 0;
    std::uint64_t configuredCaptureEpoch = 0, lastPresentTicks = 0;
    std::uint32_t width = 0, height = 0, coalescedCaptures = 0;
    std::int32_t configuredLeft = 0, configuredTop = 0;
    bool configurationPending = true, pixelsPending = false;
    bool optimizationV2 = false, continuousFrames = false;
    std::vector<wire::VideoCodec> requestedCodecs{wire::AVC};
    protocol::DesktopSessionAgreement sessionAgreement;

    void discard();
    bool resize(std::uint32_t requestedWidth, std::uint32_t requestedHeight);
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
