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
        DesktopPacing byteBound;
        byteBound.sent(1, 1, now, 256 * 1024);
        require(!byteBound.canEncode(now + 1ms)); // One large startup frame, not 16 large frames.
        require(byteBound.pendingBytes() == 256 * 1024);
        require(!byteBound.acknowledge(1, 2, 1000, 0, now + 50ms));
        require(byteBound.pendingBytes() == 256 * 1024);
        require(byteBound.acknowledge(1, 1, 1000, 0, now + 50ms));
        require(byteBound.pendingBytes() == 0 && byteBound.canEncode(now + 50ms));
        const auto initialWindow = byteBound.byteWindow();
        byteBound.sent(1, 2, now + 50ms, 32768);
        byteBound.sent(1, 3, now + 60ms, 32768);
        require(byteBound.acknowledge(1, 2, 1000, 0, now + 150ms));
        require(byteBound.acknowledge(1, 3, 1000, 0, now + 250ms));
        require(byteBound.byteWindow() < initialWindow); // Measured delivery caps queued bytes.
        byteBound.sent(1, 4, now + 300ms, 16000);
        byteBound.sent(1, 5, now + 310ms, 16000);
        require(byteBound.acknowledge(1, 5, 1000, 0, now + 500ms));
        require(byteBound.pendingBytes() == 0); // Cumulative ACK retires both frames.
        byteBound.sent(1, 6, now + 600ms, 16000);
        byteBound.reset(2);
        require(byteBound.pendingBytes() == 0 && byteBound.canEncode(now + 600ms));
        pacing.sent(1, 1, now, 16384);
        pacing.acknowledge(1, 1, 1000, 0, now + 20ms);
        require(pacing.rate().framesPerSecond == 60 && pacing.rate().bitrate == 24000000);
        const auto fast = pacing.rate();
        now += 1s;
        pacing.sent(1, 2, now, 16384);
        require(!pacing.canEncode(now + 121ms));
        pacing.acknowledge(1, 2, 1000, 0, now + 150ms);
        require(pacing.rate().framesPerSecond == fast.framesPerSecond && pacing.rate().bitrate == fast.bitrate);
        for (std::uint64_t frame = 3; frame <= 4; ++frame) {
            now += 1s;
            pacing.sent(1, frame, now, 16384);
            pacing.acknowledge(1, frame, 1000, 0, now + 150ms);
        }
        require(pacing.rate().framesPerSecond == fast.framesPerSecond && pacing.rate().bitrate < fast.bitrate);
        require(pacing.canEncode(now + 150ms));
        for (std::uint64_t frame = 5; frame < 21; ++frame) {
            pacing.sent(1, frame, now, 16384);
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
            pacing.sent(2, frame, now, 16384);
            pacing.acknowledge(2, frame, 1000, 0, now + 20ms);
        }
        require(pacing.rate().framesPerSecond == 60 && pacing.rate().bitrate == 24000000);
        for (std::uint64_t frame = 31; frame <= 90; ++frame) {
            now += 1s;
            pacing.sent(2, frame, now, 16384);
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
            jitter.sent(1, frame, now, 16384);
            jitter.acknowledge(1, frame, frame % 2 ? 100000 : 1000, 0, now + 20ms);
            require(jitter.rate().framesPerSecond >= before.framesPerSecond && jitter.rate().bitrate >= before.bitrate);
        }
        DesktopPacing highLatency;
        for (std::uint64_t frame = 1; frame <= 20; ++frame) {
            now += 1s;
            highLatency.observeRoundTrip(600ms, now);
            highLatency.sent(1, frame, now, 16384);
            highLatency.acknowledge(1, frame, 1000, 0, now + 600ms);
        }
        require(highLatency.rate().framesPerSecond == 60 && highLatency.rate().bitrate == 24000000);
        DesktopPacing narrowLink;
        for (std::uint64_t frame = 1; frame <= 3; ++frame) {
            now += 1s;
            narrowLink.observeRoundTrip(20ms, now);
            narrowLink.sent(1, frame, now, 32768);
            narrowLink.acknowledge(1, frame, 1000, 0, now + 800ms);
        }
        require(narrowLink.rate().bitrate == 512000 && narrowLink.rate().framesPerSecond == 60);
        DesktopPacing repeated;
        repeated.sent(1, 1, now, 16384);
        repeated.acknowledge(1, 1, 100000, 0, now + 20ms);
        for (int i = 0; i < 10; ++i) {
            repeated.acknowledge(1, 1, 100000, 0, now + 20ms);
        }
        require(repeated.rate().framesPerSecond == 60);
        now += 1s;
        repeated.sent(1, 2, now, 16384);
        repeated.acknowledge(1, 2, 100000, 0, now + 20ms);
        require(repeated.rate().framesPerSecond == 60);
        now += 1s;
        repeated.sent(1, 3, now, 16384);
        repeated.acknowledge(1, 3, 100000, 0, now + 20ms);
        require(repeated.rate().framesPerSecond == 45);
        DesktopPacing sharp;
        require(sharp.rate().sharp);
        require(!sharp.setQuality(DesktopPacing::Quality::kSharp));
        const auto sharpBitrate = sharp.rate().bitrate;
        for (std::uint64_t frame = 1; frame <= 3; ++frame) {
            now += 1s;
            sharp.sent(1, frame, now, 16384);
            sharp.acknowledge(1, frame, 1000, 7, now + 200ms);
        }
        require(sharp.rate().framesPerSecond == 60 && sharp.rate().bitrate < sharpBitrate);
        for (std::uint64_t frame = 4; frame <= 90; ++frame) {
            now += 1s;
            sharp.sent(1, frame, now, 16384);
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
