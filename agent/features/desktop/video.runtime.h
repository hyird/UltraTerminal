#pragma once

#include <asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <ruvia/core/EventLoop.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Process-local counters for diagnosing capture/pacing without recording pixels.
struct DesktopVideoDiagnostics {
    std::uint64_t captureCalls = 0;
    std::uint64_t pixelUpdates = 0;
    std::uint64_t cursorOnlyUpdates = 0;
    std::uint64_t captureTimeouts = 0;
    std::uint64_t pendingBeforeDeadlineCalls = 0;
    std::uint64_t backpressuredCalls = 0;
    std::uint64_t encodedFrames = 0;
    std::uint64_t earlyEncodedFrames = 0;
    std::uint64_t pixelCaptureGaps = 0;
    std::uint64_t pixelCaptureGapsGe45ms = 0;
};
struct DesktopVideoUpdate {
    std::optional<wire::VideoConfig> configuration;
    std::optional<wire::CursorUpdate> cursor;
    // Empty data means a cursor-only update; it does not advance the video ID.
    wire::VideoFrame frame;
    std::uint64_t generation = 0;
    std::uint64_t frameId = 0;
    // Local-only QPC tick of the most recent pixel snapshot (not serialized).
    std::uint64_t sourcePresentTicks = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
};
// One owner on the desktop process event loop. Capture and encoding run on a
// dedicated bounded worker. Await next() before calling it again or destroying
// the runtime. stop() interrupts pacing; an in-flight codec operation is joined.
class DesktopVideoRuntime {
  public:
    DesktopVideoRuntime(ruvia::EventLoop loop, std::uint32_t displayIndex);
    ~DesktopVideoRuntime();
    DesktopVideoRuntime(const DesktopVideoRuntime&) = delete;
    DesktopVideoRuntime& operator=(const DesktopVideoRuntime&) = delete;
    asio::awaitable<std::optional<DesktopVideoUpdate>> next(bool refresh = false);
    // Call between next() operations; display changes run on the capture worker.
    asio::awaitable<bool> resize(std::uint32_t width, std::uint32_t height);
    void stop();       // Owning event loop only.
    void invalidate(); // Discard an in-flight old frame and rebuild the source.
    void feedback(const wire::QualityFeedback& feedback);
    void observeRoundTrip(std::chrono::microseconds elapsed);
    // Owning event loop only; most recent completed capture-worker snapshot.
    DesktopVideoDiagnostics diagnostics() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
