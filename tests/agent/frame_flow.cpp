#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>

#include "features/desktop/frame_flow.runtime.h"

namespace {
using ultraterimal::agent::DesktopFrameFlow;
using namespace std::chrono_literals;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
// Finite serial link: bytes consume capacity before the RTT-delayed ACK.
// Frame sizes follow the encoder target. No instantaneous fake ACK delivery.
struct Link {
    DesktopFrameFlow flow;
    struct Ack {
        std::uint64_t frame;
        DesktopFrameFlow::Clock::time_point due;
    };
    std::deque<Ack> acks;
    DesktopFrameFlow::Clock::time_point now{1s}, wireFree{1s}, encodeAt{};
    std::uint64_t frame = 0;
    std::size_t peakFrames = 0, peakBytes = 0;
    void run(std::chrono::seconds duration, std::uint32_t bytesPerSecond, std::chrono::milliseconds rtt) {
        const auto end = now + duration;
        flow.observeRoundTrip(std::chrono::duration_cast<std::chrono::microseconds>(rtt), now);
        while (now < end) {
            while (!acks.empty() && acks.front().due <= now) {
                require(flow.acknowledge(1, acks.front().frame, 1000, 0, now, false, 128), "Link ACK rejected");
                acks.pop_front();
            }
            if (now >= encodeAt && flow.canEncode(now)) {
                const auto rate = flow.rate();
                const auto bytes = std::max(100u, rate.bitrate / (8 * rate.framesPerSecond));
                flow.sent(1, ++frame, now, bytes);
                wireFree = std::max(wireFree, now) + std::chrono::microseconds(static_cast<std::uint64_t>(bytes) * 1000000 / bytesPerSecond);
                acks.push_back({frame, wireFree + rtt + 1ms});
                encodeAt = now + std::chrono::microseconds(1000000 / rate.framesPerSecond);
                peakFrames = std::max(peakFrames, flow.pendingFrames());
                peakBytes = std::max(peakBytes, flow.pendingBytes());
            }
            now += 1ms;
        }
    }
};
} // namespace
int main() {
    try {
        auto now = DesktopFrameFlow::Clock::time_point{1s};
        DesktopFrameFlow flow;
        require(flow.rate().framesPerSecond == 60, "Healthy desktop retained the old 30 FPS ceiling");
        flow.sent(1, 1, now, 4000000);
        require(!flow.canEncode(now + 1ms), "Oversized IDR did not exhaust byte credit");
        require(!flow.acknowledge(1, 2, 1000, 0, now + 20ms, false, 128), "Future ACK accepted");
        require(!flow.acknowledge(2, 1, 1000, 0, now + 20ms, false, 128), "Wrong-generation ACK accepted");
        require(!flow.acknowledge(1, 1, 1000, 0, now + 20ms, false, 129), "Invalid receiver capacity accepted");
        require(flow.acknowledge(1, 1, 1000, 0, now + 20ms, false, 1) && flow.frameWindow() == 1, "Receiver capacity ignored");
        const auto bitrate = flow.rate().bitrate;
        require(flow.acknowledge(1, 1, 600000, 128, now + 1s, true, 1) && !flow.canEncode(now + 1s), "Repeated ACK did not pause a static desktop");
        require(flow.rate().bitrate == bitrate, "Repeated ACK changed capacity estimate");
        require(flow.acknowledge(1, 1, 1000, 0, now + 2s, false, 128) && flow.canEncode(now + 2s), "Visible static desktop did not resume");
        flow.sent(1, 2, now + 2s, 1000);
        flow.sent(1, 3, now + 2s, 1000);
        require(flow.acknowledge(1, 3, 1000, 0, now + 2030ms, false, 128) && flow.pendingBytes() == 0, "Cumulative ACK leaked credit");
        flow.sent(2, 1, now + 3s, 1000);
        require(!flow.acknowledge(1, 3, 1000, 0, now + 4s), "Retired generation affected flow");
        require(flow.acknowledge(2, 1, 1000, 0, now + 4s), "New baseline ACK failed");
        require(!flow.setGeometry(4096, 4096) && !flow.setGeometry(17, 16), "Invalid geometry accepted");
        DesktopFrameFlow cpu;
        cpu.observeEncode(40ms);
        require(cpu.rate().framesPerSecond <= 20, "Encoding pressure did not reduce capture load");
        DesktopFrameFlow decoder;
        decoder.observeRoundTrip(20ms, now);
        for (std::uint64_t id = 1; id <= 12; ++id) {
            now += 300ms;
            decoder.sent(1, id, now, 1000);
            require(decoder.acknowledge(1, id, 1000, 4, now + 21ms), "Decoder backlog ACK rejected");
        }
        require(decoder.rate().framesPerSecond < 30 && decoder.rate().bitrate == 24000000, "Decoder backlog did not reduce capture independently of network bitrate");
        DesktopFrameFlow idle;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            now += 2s;
            idle.sent(1, id, now, 1000);
            require(idle.acknowledge(1, id, 1000, 0, now + 20ms), "Idle ACK rejected");
        }
        require(idle.rate().framesPerSecond == 60 && idle.rate().bitrate == 24000000, "Static idle time was interpreted as narrow capacity");
        Link fast;
        fast.run(12s, 10000000, 20ms);
        require(fast.frame > 600 && fast.flow.rate().framesPerSecond == 60, "Fast path cannot sustain above 30 FPS");
        Link distant;
        distant.run(12s, 10000000, 600ms);
        require(distant.frame > 500 && distant.flow.rate().framesPerSecond == 60, "Stable high RTT caused unnecessary rate reduction");
        Link narrow;
        narrow.run(30s, 125000, 40ms);
        require(narrow.flow.rate().bitrate < 2000000 && narrow.flow.rate().framesPerSecond < 30, "Narrow link failed to adapt both bitrate and capture cadence");
        require(narrow.peakFrames <= 128 && narrow.peakBytes <= 4 * 1024 * 1024 + 50000, "Narrow link exceeded bounded flight");
        const auto reduced = narrow.flow.rate();
        narrow.run(25s, 10000000, 40ms);
        std::cout << "recovery_25s_bitrate=" << narrow.flow.rate().bitrate << " fps=" << narrow.flow.rate().framesPerSecond << '\n';
        require(narrow.flow.rate().bitrate > reduced.bitrate * 8 && narrow.flow.rate().framesPerSecond >= 45, "Capacity recovery remained too slow");
        narrow.run(30s, 125000, 40ms);
        require(narrow.flow.rate().bitrate < 2000000 && narrow.flow.rate().framesPerSecond < 30, "Recovery ignored renewed congestion");
        DesktopFrameFlow stalled;
        stalled.sent(1, 1, now, 1000);
        require(!stalled.canEncode(now + 1s), "Missing ACK did not stop further encoding");
        require(stalled.acknowledge(1, 1, 1000, 0, now + 2s) && stalled.canEncode(now + 2s), "ACK did not resume a stalled window");
        std::cout << "Receiver credit, finite-capacity congestion/recovery, high RTT, idle, CPU pressure and generation safety passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
