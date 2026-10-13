#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace ultraterimal::agent {
// framesPerSecond is the validated nonzero Flow rate. started precedes
// acquisition/readback, so an overrun does not buy another idle period.
inline std::chrono::steady_clock::time_point nextDesktopFrameAt(std::chrono::steady_clock::time_point previous,
                                                                std::chrono::steady_clock::time_point started,
                                                                std::uint32_t framesPerSecond, bool continuous, bool refresh) {
    const auto period = std::chrono::microseconds(1000000 / framesPerSecond);
    // Early acquisition may cross E. Keep its due slot; a forced baseline
    // instead starts a new cadence and must not extend the previous slot.
    if (!refresh) {
        started = std::max(started, previous);
    }
    if (continuous && previous != std::chrono::steady_clock::time_point{} && started >= previous && started - previous < period) {
        return previous + period;
    }
    return started + period;
}
// Unchanged pictures only keep decoding and presentation continuous. A faster
// adaptive content cadence must not multiply their idle encode and link cost.
inline constexpr std::uint32_t kMaximumRepeatedDesktopFramesPerSecond = 30;
struct DesktopFrameSlots {
    std::chrono::steady_clock::time_point content, repeated;
};
// A repeat occupies its own slot on the repeat grid; the next changed picture
// is one content period after it. At or below the repeat ceiling both slots
// coincide, so the cadence equals nextDesktopFrameAt.
inline DesktopFrameSlots nextDesktopFrameSlots(DesktopFrameSlots previous, std::chrono::steady_clock::time_point started,
                                               std::uint32_t framesPerSecond, bool continuous, bool refresh, bool repeated) {
    const auto content = nextDesktopFrameAt(repeated ? previous.repeated : previous.content, started, framesPerSecond, continuous, refresh);
    const auto repeatFramesPerSecond = std::min(framesPerSecond, kMaximumRepeatedDesktopFramesPerSecond);
    return {content, content + std::chrono::microseconds(1000000 / repeatFramesPerSecond - 1000000 / framesPerSecond)};
}
} // namespace ultraterimal::agent
