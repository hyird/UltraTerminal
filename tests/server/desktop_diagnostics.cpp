#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "features/desktop/desktop_diagnostics.types.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Desktop encoding statistics differ");
    }
}
}
int main() {
    try {
        ultraterimal::server::DesktopEncodingWindow window;
        require(window.summary().samples == 0);
        window.observe(0);
        require(window.summary().samples == 0);
        window.observe(7);
        auto result = window.summary();
        require(result.samples == 1 && result.meanMicroseconds == 7 && result.p95Microseconds == 7 && result.p99Microseconds == 7 && result.maximumMicroseconds == 7);
        window.clear();
        for (std::uint32_t value = 1; value <= 1024; ++value) {
            window.observe(value);
        }
        result = window.summary();
        require(result.samples == 1024 && result.meanMicroseconds == 512 && result.p95Microseconds == 973 && result.p99Microseconds == 1014 && result.maximumMicroseconds == 1024);
        window.observe(2000); // Evicts 1, rather than growing the allocation.
        window.observe(0);    // Missing telemetry neither biases nor evicts samples.
        result = window.summary();
        require(result.samples == 1024 && result.meanMicroseconds == 514 && result.p95Microseconds == 974 && result.p99Microseconds == 1015 && result.maximumMicroseconds == 2000);
        for (unsigned i = 0; i < 2048; ++i) {
            window.observe(9);
        }
        result = window.summary();
        require(result.samples == 1024 && result.meanMicroseconds == 9 && result.maximumMicroseconds == 9);
        window.clear();
        require(window.summary().samples == 0);
        for (unsigned i = 0; i < 1024; ++i) {
            window.observe(std::numeric_limits<std::uint32_t>::max());
        }
        result = window.summary();
        require(result.meanMicroseconds == std::numeric_limits<std::uint32_t>::max() && result.p95Microseconds == result.meanMicroseconds && result.p99Microseconds == result.meanMicroseconds);
        ultraterimal::server::DesktopVisibleFrameDiagnostics raf;
        raf.observe(120, 48000, 115000, 1, 27500);
        raf.observe(120, 50000, 97000, 2, 23000);
        require(raf.reports == 2 && raf.latestSamples == 120 && raf.latestP95Microseconds == 50000 && raf.latestP99Microseconds == 97000 && raf.latestLongGaps == 2);
        require(raf.latestFpsMilli == 23000 && raf.minimumFpsMilli == 23000 && raf.maximumFpsMilli == 27500);
        require(raf.maximumP95Microseconds == 50000 && raf.maximumP99Microseconds == 115000 && raf.maximumLongGaps == 2);
        raf.clear();
        require(raf.reports == 0 && raf.latestSamples == 0 && raf.minimumFpsMilli == 0 && raf.maximumP95Microseconds == 0 && raf.maximumLongGaps == 0);
        std::cout << "Encoding and rAF telemetry absence, quantiles, bounded windows and reset passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
