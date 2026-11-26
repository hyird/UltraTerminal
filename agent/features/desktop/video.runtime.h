#pragma once
#include <asio/awaitable.hpp>
#include <blocking_pool.h>
#include <chrono>
#include <cstdint>
#include <event_loop.h>
#include <memory>
#include <optional>

#include "video.types.h"

namespace ultraterimal::agent {
// One owner on the desktop process event loop. Capture and encoding run on a
// dedicated bounded worker. Await next() before calling it again or destroying
// the runtime. stop() interrupts pacing; an in-flight codec operation is joined.
class DesktopVideoRuntime {
  public:
    DesktopVideoRuntime(ruvia::event_loop loop, ruvia::blocking_pool& pool, std::uint32_t displayIndex, std::uint32_t serverOptimizationContract = 0, bool serverContinuousFrames = false);
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
    void observeRoundTrip(std::chrono::microseconds elapsed);
    // Owning event loop only; most recent completed capture-worker snapshot.
    DesktopVideoDiagnostics diagnostics() const;

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
