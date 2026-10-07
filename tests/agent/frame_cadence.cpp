#include <chrono>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

#include "features/desktop/frame_cadence.runtime.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using ultraterimal::agent::nextDesktopFrameAt;
    try {
        const auto origin = Clock::time_point(1s);
        const auto due = nextDesktopFrameAt({}, origin, 30, true, true);
        require(due == origin + 33333us, "Initial frame did not establish the requested period");
        for (const bool continuous : {false, true}) {
            const auto afterSlowCapture = nextDesktopFrameAt(due, due, 30, continuous, false);
            // Map(30ms) + encode(18ms) already exceed one period. The next
            // job is due immediately, rather than after another 15ms wait.
            require(afterSlowCapture == due + 33333us && afterSlowCapture < due + 48ms,
                    "Slow capture added an extra pacing wait");
            require(nextDesktopFrameAt(due, origin + 20ms, 30, continuous, false) == due + 33333us,
                    "Early acquisition shortened the cadence below the FPS ceiling");
            require(nextDesktopFrameAt(origin + 100ms, origin + 20ms, 30, continuous, true) == origin + 53333us,
                    "Forced recovery extended the old frame deadline");
        }
        require(nextDesktopFrameAt(due, due + 1ms, 30, true, false) == due + 33333us,
                "Normal timer lateness caused continuous cadence drift");
        require(nextDesktopFrameAt(due, due + 90ms, 30, true, false) == due + 90ms + 33333us,
                "Long stall attempted to burst through missed slots");
        require(nextDesktopFrameAt({}, origin, 20, true, true) == origin + 50ms,
                "Adaptive FPS did not change the period");
        std::cout << "Capture overlap, overruns, recovery and bounded cadence passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
