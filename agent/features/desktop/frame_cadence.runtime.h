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
} // namespace ultraterimal::agent
