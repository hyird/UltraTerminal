#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
struct VideoEncoderConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bitrate = 4000000;
    std::uint32_t framesPerSecond = 30;
    bool sharp = false;
    std::vector<wire::VideoCodec> codecs{wire::AVC};
};
enum class VideoEncodeError { kInvalidPixels,
                              kInvalidTimestamp,
                              kCodecFailure,
                              kFrameSkipped,
                              kFrameTooLarge,
                              kGpuUnavailable };
struct EncodedVideoFrame {
    std::vector<std::uint8_t> annexB;
    std::string codec; // WebCodecs codec string derived from emitted headers.
    std::uint64_t timestampMicroseconds = 0;
    bool keyFrame = false;
    bool baselineReset = false; // Encoder failover or codec change requires a new generation.
};
struct VideoFrameDescription {
    std::string codec;
    bool keyFrame = false;
};
struct DesktopVideoStages {
    std::optional<std::uint32_t> workerQueue, acquireWait, sourceAge, pacingCreditWait;
    std::optional<std::uint32_t> gpuSyncWait, prepareCpu, videoEncode;
    std::uint32_t coalescedCaptures = 0;
    static std::uint32_t microseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), 0, 60000000));
    }
};
// Process-local counters, with no screen or input content.
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
    bool lastJobAcquired = false;
    std::uint64_t pixelCaptureGaps = 0;
    std::uint64_t pixelCaptureGapsGe45ms = 0;
};
struct DesktopVideoUpdate {
    std::optional<wire::DesktopFlowSample> flowSample; // Input-channel diagnostics.
    std::optional<wire::VideoConfig> configuration;
    std::optional<wire::CursorUpdate> cursor;
    wire::VideoFrame frame;
    std::uint64_t generation = 0;
    std::uint64_t frameId = 0;
    std::uint64_t baseFrameId = 0;
    std::uint64_t changedPixels = 0;
    // Local-only QPC tick of the most recent pixel snapshot (not serialized).
    std::uint64_t sourcePresentTicks = 0;
    std::uint32_t workerDispatchMicroseconds = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
};
} // namespace ultraterimal::agent
