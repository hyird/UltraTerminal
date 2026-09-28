#include <chrono>
#include <iostream>
#include <stdexcept>

#include "features/desktop/pacing.runtime.h"

int main() {
    using namespace std::chrono_literals;
    using ultraterimal::agent::DesktopPacing;
    const auto requireAt = [](bool value, int line) {
        if (!value) {
            throw std::runtime_error("Desktop pacing invariant failed at line " + std::to_string(line));
        }
    };
#define require(value) requireAt((value), __LINE__)
    try {
        DesktopPacing pacing;
        auto now = DesktopPacing::Clock::time_point{} + 1s;
        pacing.sent(1, 1, now);
        pacing.acknowledge(1, 1, 1000, 0, now + 20ms);
        require(pacing.rate().framesPerSecond == 60 && pacing.rate().bitrate == 24000000);
        const auto fast = pacing.rate();
        now += 1s;
        pacing.sent(1, 2, now);
        require(!pacing.canEncode(now + 121ms));
        pacing.acknowledge(1, 2, 1000, 0, now + 150ms);
        require(pacing.rate().framesPerSecond == fast.framesPerSecond && pacing.rate().bitrate == fast.bitrate);
        for (std::uint64_t frame = 3; frame <= 4; ++frame) {
            now += 1s;
            pacing.sent(1, frame, now);
            pacing.acknowledge(1, frame, 1000, 0, now + 150ms);
        }
        require(pacing.rate().framesPerSecond == fast.framesPerSecond && pacing.rate().bitrate < fast.bitrate);
        require(pacing.canEncode(now + 150ms));
        for (std::uint64_t frame = 5; frame < 21; ++frame) {
            pacing.sent(1, frame, now);
        }
        require(!pacing.canEncode(now));
        pacing.acknowledge(2, 18, 1000, 0, now + 20ms);
        require(!pacing.canEncode(now));
        pacing.acknowledge(1, 999, 1000, 0, now + 20ms);
        require(!pacing.canEncode(now));
        pacing.reset(2);
        require(pacing.canEncode(now));
        for (std::uint64_t frame = 1; frame <= 30; ++frame) {
            now += 1s;
            pacing.sent(2, frame, now);
            pacing.acknowledge(2, frame, 1000, 0, now + 20ms);
        }
        require(pacing.rate().framesPerSecond == 60 && pacing.rate().bitrate == 24000000);
        for (std::uint64_t frame = 31; frame <= 90; ++frame) {
            now += 1s;
            pacing.sent(2, frame, now);
            pacing.acknowledge(2, frame, 100000, 7, now + 200ms);
        }
        require(pacing.rate().framesPerSecond == 10 && pacing.rate().bitrate == 512000);
        const auto settled = pacing.rate();
        require(pacing.acknowledge(2, 90, 0, 0, now + 1s, true));
        require(!pacing.canEncode(now + 1s));
        require(!pacing.acknowledge(1, 90, 0, 0, now + 2s, false));
        require(!pacing.acknowledge(2, 89, 0, 0, now + 2s, false));
        require(!pacing.acknowledge(2, 91, 0, 0, now + 2s, false));
        require(!pacing.canEncode(now + 2s));
        require(pacing.acknowledge(2, 90, 0, 0, now + 3s, false));
        require(pacing.canEncode(now + 3s));
        require(pacing.rate().framesPerSecond == settled.framesPerSecond && pacing.rate().bitrate == settled.bitrate);
        DesktopPacing jitter;
        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            now += 1s;
            const auto before = jitter.rate();
            jitter.sent(1, frame, now);
            jitter.acknowledge(1, frame, frame % 2 ? 100000 : 1000, 0, now + 20ms);
            require(jitter.rate().framesPerSecond >= before.framesPerSecond && jitter.rate().bitrate >= before.bitrate);
        }
        DesktopPacing highLatency;
        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            now += 1s;
            highLatency.sent(1, frame, now);
            highLatency.acknowledge(1, frame, 1000, 0, now + 600ms);
        }
        require(highLatency.rate().framesPerSecond == 60 && highLatency.rate().bitrate == 24000000);
        DesktopPacing repeated;
        repeated.sent(1, 1, now);
        repeated.acknowledge(1, 1, 100000, 0, now + 20ms);
        for (int i = 0; i < 10; ++i) {
            repeated.acknowledge(1, 1, 100000, 0, now + 20ms);
        }
        require(repeated.rate().framesPerSecond == 60);
        now += 1s;
        repeated.sent(1, 2, now);
        repeated.acknowledge(1, 2, 100000, 0, now + 20ms);
        require(repeated.rate().framesPerSecond == 60);
        now += 1s;
        repeated.sent(1, 3, now);
        repeated.acknowledge(1, 3, 100000, 0, now + 20ms);
        require(repeated.rate().framesPerSecond == 45);
        DesktopPacing sharp;
        require(sharp.rate().sharp);
        require(!sharp.setQuality(DesktopPacing::Quality::kSharp));
        const auto sharpBitrate = sharp.rate().bitrate;
        for (std::uint64_t frame = 1; frame <= 3; ++frame) {
            now += 1s;
            sharp.sent(1, frame, now);
            sharp.acknowledge(1, frame, 1000, 7, now + 200ms);
        }
        require(sharp.rate().framesPerSecond == 60 && sharp.rate().bitrate < sharpBitrate);
        for (std::uint64_t frame = 4; frame <= 90; ++frame) {
            now += 1s;
            sharp.sent(1, frame, now);
            sharp.acknowledge(1, frame, 1000, 7, now + 200ms);
        }
        require(sharp.rate().framesPerSecond == 60 && sharp.rate().bitrate == 512000);
        require(sharp.setQuality(DesktopPacing::Quality::kSmooth));
        require(sharp.rate().bitrate == 8000000);
        require(sharp.setQuality(DesktopPacing::Quality::kBalanced));
        require(sharp.rate().bitrate == 16000000);
        std::cout << "Pacing growth, quality preferences, congestion, queue bounds, generation and recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
