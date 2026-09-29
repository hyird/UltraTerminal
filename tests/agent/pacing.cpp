#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "features/desktop/pacing.runtime.h"

namespace {
using ultraterimal::agent::DesktopPacing;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void congestionSample(DesktopPacing& pacing, DesktopPacing::Clock::time_point& now, std::uint64_t& frame, std::uint32_t queueDepth = 3,
                      std::uint32_t decodeMicroseconds = 1000) {
    now += std::chrono::seconds(1);
    pacing.sent(1, frame, now, 16384);
    require(pacing.acknowledge(1, frame, decodeMicroseconds, queueDepth, now + std::chrono::milliseconds(20)), "Valid congestion feedback rejected");
    ++frame;
}
} // namespace
int main() {
    using namespace std::chrono_literals;
    try {
        auto now = DesktopPacing::Clock::time_point{} + 1s;

        DesktopPacing geometry;
        require(geometry.clearBitrateAt60() == 8000000, "1080p60 clarity budget must be 8 Mbps");
        require(geometry.minimumBitrate(60) == 8000000 && geometry.minimumBitrate(30) == 4000000 && geometry.minimumBitrate(10) == 1333334,
                "1080p per-frame budget did not scale with FPS");
        require(geometry.setGeometry(2560, 1080), "Accept 2560x1080 geometry");
        require(geometry.clearBitrateAt60() == 10666667 && geometry.minimumBitrate(30) == 5333334,
                "Wide-screen clarity budget must scale by pixel area");
        require(geometry.setGeometry(3840, 2160) && geometry.clearBitrateAt60() == 32000000,
                "4K clarity budget must scale by pixel area");
        require(geometry.setGeometry(2160, 3840) && geometry.clearBitrateAt60() == 32000000,
                "Valid portrait geometry was rejected or mis-budgeted");
        require(geometry.setGeometry(2160, 4096) && geometry.clearBitrateAt60() == 34133334,
                "Maximum-area portrait geometry was rejected or mis-budgeted");
        require(geometry.setGeometry(4096, 2160) && geometry.clearBitrateAt60() <= DesktopPacing::kMaximumVideoBitrate,
                "Maximum encoder geometry exceeded legal bitrate");
        const auto legalGeometry = geometry.geometry();
        for (const auto [width, height] : {std::pair{0u, 1080u}, std::pair{1920u, 1081u}, std::pair{4098u, 1080u},
                                           std::pair{2162u, 4096u}, std::pair{2160u, 4098u},
                                           std::pair{std::numeric_limits<std::uint32_t>::max(), 2160u}}) {
            require(!geometry.setGeometry(width, height), "Reject invalid or overflowing geometry");
            require(geometry.geometry() == legalGeometry, "Invalid geometry changed the active budget");
        }
        for (const auto quality : {DesktopPacing::Quality::kSmooth, DesktopPacing::Quality::kBalanced, DesktopPacing::Quality::kSharp}) {
            require(geometry.setQuality(quality), "Quality mode change was not applied");
            require(geometry.rate().framesPerSecond == 60 && geometry.rate().bitrate >= geometry.clearBitrateAt60() &&
                        geometry.rate().bitrate <= DesktopPacing::kMaximumVideoBitrate,
                    "Quality target violated 4K60 clarity or encoder limit");
        }
        require(geometry.setGeometry(2560, 1080), "Return to wide geometry");
        require(geometry.setQuality(DesktopPacing::Quality::kSmooth) && geometry.rate().bitrate == 10666667,
                "Smooth preference must not undercut the 60 FPS clarity budget");
        require(geometry.setGeometry(1920, 1080) && geometry.rate().bitrate == 8000000,
                "Returning to 1080p should restore the selected healthy bitrate target");
        DesktopPacing modeSwitch;
        for (const auto [quality, bitrate] : {std::pair{DesktopPacing::Quality::kSmooth, 8000000u},
                                              std::pair{DesktopPacing::Quality::kBalanced, 16000000u},
                                              std::pair{DesktopPacing::Quality::kSharp, 24000000u}}) {
            require(modeSwitch.setQuality(quality) && modeSwitch.rate().bitrate == bitrate && modeSwitch.rate().framesPerSecond == 60,
                    "Quality preference mode failed to restore its 1080p healthy target");
        }

        DesktopPacing minimumGeometry;
        require(minimumGeometry.setGeometry(16, 16) && minimumGeometry.clearBitrateAt60() == DesktopPacing::kMinimumVideoBitrate &&
                    minimumGeometry.minimumBitrate(10) == DesktopPacing::kMinimumVideoBitrate &&
                    minimumGeometry.minimumBitrate(60) == DesktopPacing::kMinimumVideoBitrate,
                "Smallest valid geometry fell below the encoder's legal bitrate floor");
        std::uint64_t minimumFrame = 1;
        require(minimumGeometry.setQuality(DesktopPacing::Quality::kSmooth), "Minimum-size smooth mode was not applied");
        for (int i = 0; i < 120; ++i) {
            congestionSample(minimumGeometry, now, minimumFrame);
            require(minimumGeometry.rate().bitrate >= DesktopPacing::kMinimumVideoBitrate &&
                        minimumGeometry.rate().bitrate <= DesktopPacing::kMaximumVideoBitrate &&
                        minimumGeometry.rate().bitrate >= minimumGeometry.minimumBitrate(minimumGeometry.rate().framesPerSecond),
                    "Minimum-size congestion produced an encoder-illegal rate");
        }
        require(minimumGeometry.rate().framesPerSecond == DesktopPacing::kMinimumFramesPerSecond &&
                    minimumGeometry.rate().bitrate == DesktopPacing::kMinimumVideoBitrate,
                "Sustained minimum-size congestion did not settle at legal encoder limits");
        for (int i = 0; i < 160; ++i) {
            now += 1s;
            minimumGeometry.sent(1, minimumFrame, now, 1024);
            require(minimumGeometry.acknowledge(1, minimumFrame++, 1000, 0, now + 20ms), "Minimum-size recovery ACK rejected");
            require(minimumGeometry.rate().bitrate >= DesktopPacing::kMinimumVideoBitrate &&
                        minimumGeometry.rate().bitrate <= DesktopPacing::kMaximumVideoBitrate &&
                        minimumGeometry.rate().bitrate >= minimumGeometry.minimumBitrate(minimumGeometry.rate().framesPerSecond),
                    "Minimum-size recovery produced an encoder-illegal rate");
        }
        for (const auto [quality, bitrate] : {std::pair{DesktopPacing::Quality::kBalanced, 16000000u},
                                              std::pair{DesktopPacing::Quality::kSharp, 24000000u},
                                              std::pair{DesktopPacing::Quality::kSmooth, 8000000u}}) {
            require(minimumGeometry.setQuality(quality) && minimumGeometry.rate().bitrate == bitrate &&
                        minimumGeometry.rate().bitrate >= DesktopPacing::kMinimumVideoBitrate &&
                        minimumGeometry.rate().bitrate <= DesktopPacing::kMaximumVideoBitrate,
                    "Minimum-size quality switch escaped legal encoder rates");
        }

        DesktopPacing byteBound;
        byteBound.sent(1, 1, now, 256 * 1024);
        require(!byteBound.canEncode(now + 1ms), "Byte window admitted another oversized frame");
        require(byteBound.pendingBytes() == 256 * 1024, "Pending bytes not tracked");
        require(!byteBound.acknowledge(1, 2, 1000, 0, now + 50ms), "Unknown ACK accepted");
        require(byteBound.pendingBytes() == 256 * 1024, "Unknown ACK retired bytes");
        require(byteBound.acknowledge(1, 1, 1000, 0, now + 50ms), "Valid ACK rejected");
        require(byteBound.pendingBytes() == 0 && byteBound.canEncode(now + 50ms), "ACK did not reopen admission");

        DesktopPacing shrinkingWindow;
        shrinkingWindow.sent(1, 1, now, 1);
        require(shrinkingWindow.acknowledge(1, 1, 1000, 0, now + 20ms), "Could not establish display baseline");
        const auto initialWindow = shrinkingWindow.byteWindow();
        shrinkingWindow.sent(1, 2, now + 25ms, 30000);
        shrinkingWindow.sent(1, 3, now + 30ms, 30000);
        require(shrinkingWindow.canEncode(now + 31ms), "Frame was not admitted under the original window");
        require(shrinkingWindow.acknowledge(1, 2, 1000, 0, now + 50ms), "Pipelined ACK rejected");
        require(shrinkingWindow.acknowledge(1, 3, 1000, 0, now + 80ms), "Second pipelined ACK rejected");
        require(shrinkingWindow.byteWindow() < initialWindow, "Measured delivery did not shrink the byte window");
        // The encoder may finish work admitted before that shrink. sent() must
        // enforce only the hard safety ceiling, not mistake this for corruption.
        shrinkingWindow.sent(1, 4, now + 81ms, 128 * 1024);
        require(shrinkingWindow.pendingBytes() == 128 * 1024 && !shrinkingWindow.canEncode(now + 82ms),
                "Shrunk window failed to apply backpressure to subsequent work");

        DesktopPacing rate;
        require(rate.rate().framesPerSecond == 60 && rate.rate().bitrate >= rate.clearBitrateAt60(), "Initial mode missed its clarity target");
        std::uint64_t frame = 1;
        rate.sent(1, frame, now, 16384);
        require(rate.acknowledge(1, frame++, 1000, 0, now + 20ms), "Initial healthy ACK rejected");
        // Sustained congestion reduces bitrate only down to the current FPS
        // budget, then trades FPS for the same minimum bits/frame.
        for (int i = 0; i < 60; ++i) {
            congestionSample(rate, now, frame);
            require(rate.rate().bitrate >= rate.minimumBitrate(rate.rate().framesPerSecond), "Congestion broke the per-frame clarity budget");
        }
        require(rate.rate().framesPerSecond < 60 && rate.rate().framesPerSecond >= DesktopPacing::kMinimumFramesPerSecond,
                "Network congestion did not reduce FPS after reaching the 60 FPS bitrate budget");
        require(rate.rate().bitrate > 512000, "Congestion incorrectly crushed the clear-frame budget to 512 Kbps");
        const auto congested = rate.rate();
        for (int i = 0; i < 160; ++i) {
            now += 1s;
            rate.sent(1, frame, now, 1024);
            require(rate.acknowledge(1, frame++, 1000, 0, now + 20ms), "Healthy recovery ACK rejected");
            require(rate.rate().bitrate >= rate.minimumBitrate(rate.rate().framesPerSecond), "Recovery raised FPS before funding its frame budget");
        }
        require(rate.rate().framesPerSecond == 60 && rate.rate().bitrate >= rate.clearBitrateAt60(), "Stable recovery did not restore a funded 60 FPS target");
        require(congested.framesPerSecond < 60, "Congested snapshot unexpectedly retained 60 FPS");

        DesktopPacing narrow;
        now += 1s;
        narrow.observeRoundTrip(20ms, now);
        std::uint64_t narrowFrame = 1;
        for (int i = 0; i < 100; ++i) {
            congestionSample(narrow, now, narrowFrame, 7, 100000);
            require(narrow.rate().framesPerSecond >= DesktopPacing::kMinimumFramesPerSecond &&
                        narrow.rate().bitrate >= narrow.minimumBitrate(narrow.rate().framesPerSecond),
                    "Very narrow link escaped bounded minimum-FPS backpressure");
            require(narrow.pendingFrames() == 0 && narrow.pendingBytes() == 0, "ACKed narrow-link data remained queued");
        }
        require(narrow.rate().framesPerSecond == DesktopPacing::kMinimumFramesPerSecond,
                "Decoder/network overload did not reach the bounded minimum FPS");
        require(narrow.rate().bitrate > 512000, "Narrow link discarded the minimum per-frame quality budget");

        DesktopPacing belowMinimumCapacity;
        belowMinimumCapacity.observeRoundTrip(20ms, now);
        belowMinimumCapacity.sent(1, 1, now, 1000);
        belowMinimumCapacity.sent(1, 2, now + 1ms, 1000);
        require(belowMinimumCapacity.acknowledge(1, 1, 1000, 0, now + 20ms), "Narrow-capacity baseline ACK rejected");
        belowMinimumCapacity.sent(1, 3, now + 21ms, 1000);
        require(belowMinimumCapacity.acknowledge(1, 2, 1000, 0, now + 40ms), "Narrow-capacity throughput ACK rejected");
        belowMinimumCapacity.sent(1, 4, now + 41ms, 1000);
        require(belowMinimumCapacity.acknowledge(1, 3, 1000, 3, now + 600ms), "Narrow-capacity congestion ACK rejected");
        require(belowMinimumCapacity.acknowledge(1, 4, 1000, 3, now + 620ms), "Second narrow-capacity congestion ACK rejected");
        belowMinimumCapacity.sent(1, 5, now + 621ms, 1000);
        require(belowMinimumCapacity.acknowledge(1, 5, 1000, 3, now + 640ms), "Third narrow-capacity congestion ACK rejected");
        require(belowMinimumCapacity.rate().framesPerSecond == DesktopPacing::kMinimumFramesPerSecond &&
                    belowMinimumCapacity.rate().bitrate == belowMinimumCapacity.minimumBitrate(DesktopPacing::kMinimumFramesPerSecond),
                "Sub-floor link should retain only the minimum-FPS frame budget");
        belowMinimumCapacity.sent(1, 6, now + 641ms, 128 * 1024);
        require(!belowMinimumCapacity.canEncode(now + 642ms) && belowMinimumCapacity.pendingFrames() == 1 &&
                    belowMinimumCapacity.pendingFrames() <= DesktopPacing::kMaximumInFlightFrames,
                "Sub-floor link must wait with bounded in-flight work rather than build a queue");

        DesktopPacing decoderBound;
        const auto initialRate = decoderBound.rate();
        std::uint64_t decoderFrame = 1;
        for (int i = 0; i < 3; ++i) {
            now += 1s;
            decoderBound.sent(1, decoderFrame, now, 16384);
            decoderBound.acknowledge(1, decoderFrame++, 100000, 0, now + 20ms);
        }
        require(decoderBound.rate().framesPerSecond < initialRate.framesPerSecond &&
                    decoderBound.rate().bitrate >= decoderBound.minimumBitrate(decoderBound.rate().framesPerSecond),
                "Decoder overload must reduce FPS without violating bits per frame");

        DesktopPacing stale;
        stale.sent(1, 1, now, 1024);
        require(!stale.acknowledge(2, 1, 1000, 0, now + 20ms), "Stale generation feedback accepted");
        require(!stale.acknowledge(1, 2, 1000, 0, now + 20ms), "Unknown future frame feedback accepted");
        stale.reset(2);
        require(stale.pendingBytes() == 0 && stale.pendingFrames() == 0 && stale.canEncode(now + 1ms), "Generation reset retained old work");

        DesktopPacing background;
        const auto visibleRate = background.rate();
        background.sent(1, 1, now, 16384);
        require(background.acknowledge(1, 1, 100000, 7, now + 20ms, true), "Background ACK rejected");
        require(background.background() && !background.canEncode(now + 21ms) && background.rate().bitrate == visibleRate.bitrate,
                "Background feedback should pause work without adapting rate");
        require(background.acknowledge(1, 1, 0, 0, now + 30ms, false), "Repeated ACK visibility update rejected");
        require(!background.background() && background.rate().bitrate == visibleRate.bitrate, "Repeated ACK adjusted the rate");

        DesktopPacing repeated;
        repeated.sent(1, 1, now, 16384);
        repeated.acknowledge(1, 1, 100000, 0, now + 20ms);
        const auto once = repeated.rate();
        for (int i = 0; i < 10; ++i) {
            require(repeated.acknowledge(1, 1, 100000, 0, now + 20ms), "Repeated ACK rejected");
        }
        require(repeated.rate().framesPerSecond == once.framesPerSecond && repeated.rate().bitrate == once.bitrate,
                "Repeated ACK changed adaptation state");

        DesktopPacing boundedFrames;
        for (std::size_t i = 0; i < DesktopPacing::kMaximumInFlightFrames; ++i) {
            boundedFrames.sent(1, i + 1, now, 1);
        }
        require(boundedFrames.pendingFrames() == DesktopPacing::kMaximumInFlightFrames, "Hard frame safety bound was not reached");
        bool hardLimit = false;
        try {
            boundedFrames.sent(1, DesktopPacing::kMaximumInFlightFrames + 1, now, 1);
        } catch (const std::logic_error&) {
            hardLimit = true;
        }
        require(hardLimit, "Hard 128-frame safety limit was exceeded");

        // Event-driven 600 ms RTT simulation. Sends occur every 16,667 us and
        // each display ACK is queued for send+600 ms. The scheduler always
        // processes the earliest event, with ACKs winning timestamp ties.
        // This is a pacing unit simulation, not a network measurement.
        DesktopPacing highLatency;
        highLatency.observeRoundTrip(600ms, now);
        require(highLatency.frameWindow() == 40, "600 ms RTT did not expand the ACK window to the required flight size");
        struct ScheduledAck {
            DesktopPacing::Clock::time_point due;
            std::uint64_t frame;
        };
        constexpr auto frameInterval = 16667us;
        constexpr std::size_t frameBytes = 512;
        const auto simulationStart = now;
        const auto simulationEnd = simulationStart + 6s;
        auto currentTime = simulationStart;
        auto nextSendAt = simulationStart;
        std::uint64_t nextFrame = 1;
        std::uint64_t lastAcknowledgedFrame = 0;
        std::size_t sentFrames = 0;
        std::size_t acknowledgedFrames = 0;
        std::size_t maximumInFlight = 0;
        std::deque<ScheduledAck> acknowledgements;
        while (nextSendAt <= simulationEnd || (!acknowledgements.empty() && acknowledgements.front().due <= simulationEnd)) {
            const bool acknowledgeFirst = !acknowledgements.empty() && acknowledgements.front().due <= nextSendAt;
            const auto eventAt = acknowledgeFirst ? acknowledgements.front().due : nextSendAt;
            if (eventAt > simulationEnd) {
                break;
            }
            require(eventAt >= currentTime, "High-RTT event time moved backwards");
            currentTime = eventAt;
            if (acknowledgeFirst) {
                const auto ack = acknowledgements.front();
                require(ack.frame == lastAcknowledgedFrame + 1, "High-RTT simulation skipped or reordered an ACK");
                require(highLatency.acknowledge(1, ack.frame, 1000, 0, currentTime), "High-RTT display ACK rejected");
                lastAcknowledgedFrame = ack.frame;
                ++acknowledgedFrames;
                acknowledgements.pop_front();
            } else {
                require(highLatency.canEncode(currentTime), "High-RTT path stalled before sustaining 60 FPS");
                highLatency.sent(1, nextFrame, currentTime, frameBytes);
                acknowledgements.push_back({currentTime + 600ms, nextFrame});
                ++nextFrame;
                ++sentFrames;
                nextSendAt += frameInterval;
                maximumInFlight = std::max(maximumInFlight, highLatency.pendingFrames());
            }
            require(highLatency.pendingFrames() <= highLatency.frameWindow() &&
                        highLatency.pendingFrames() <= DesktopPacing::kMaximumInFlightFrames &&
                        highLatency.byteWindow() <= 2 * 1024 * 1024 &&
                        highLatency.pendingBytes() <= highLatency.byteWindow() &&
                        highLatency.pendingBytes() <= 2 * 1024 * 1024 &&
                        highLatency.pendingFrames() == sentFrames - acknowledgedFrames &&
                        acknowledgements.size() == highLatency.pendingFrames(),
                    "High-RTT simulation exceeded a bound or lost an in-flight frame");
        }
        require(sentFrames == 360 && acknowledgedFrames > 300 && maximumInFlight > 16 &&
                    maximumInFlight <= DesktopPacing::kMaximumInFlightFrames && highLatency.rate().framesPerSecond == 60,
                "Several-second high-RTT simulation did not sustain 60 FPS beyond the old 16-frame window");

        DesktopPacing ageBound;
        ageBound.sent(1, 1, now, 1024);
        ageBound.acknowledge(1, 1, 1000, 0, now + 50ms);
        ageBound.sent(1, 2, now + 60ms, 1024);
        require(!ageBound.canEncode(now + 211ms), "Frame age limit did not stop stale work");
        std::cout << "Pacing geometry budgets, FPS adaptation, high-RTT flight, hard bounds and recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
