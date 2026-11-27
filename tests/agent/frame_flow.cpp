#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "features/desktop/frame_flow.runtime.h"

namespace {
using ultraterimal::agent::DesktopFrameFlow;
using FrameType = DesktopFrameFlow::FrameType;
using namespace std::chrono_literals;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
struct Traffic {
    FrameType type = FrameType::kVideo;
    std::uint32_t fixedBytes = 0; // Zero models a VBV codec; sharp traffic never shrinks with target bitrate.
    std::uint64_t changedPixels = 1920 * 1080;
    std::uint32_t decodeMicroseconds = 1000, queueDepth = 0;
    unsigned delayedFrames = 0;
    std::chrono::microseconds feedbackDelay{};
};
// Finite serial link: actual bytes consume capacity before the RTT-delayed ACK.
// Feedback is FIFO even when delayed; no instantaneous fake ACK delivery.
struct Link {
    DesktopFrameFlow flow;
    struct Ack {
        std::uint64_t frame;
        DesktopFrameFlow::Clock::time_point due;
        std::uint32_t decodeMicroseconds, queueDepth;
    };
    std::deque<Ack> acks;
    DesktopFrameFlow::Clock::time_point now{1s}, wireFree{1s}, encodeAt{};
    std::uint64_t frame = 0;
    std::size_t peakFrames = 0, peakBytes = 0;
    std::vector<DesktopFrameFlow::Clock::time_point> videoTimes;
    std::uint32_t minimumBitrate = DesktopFrameFlow::kTargetVideoBitrate;
    void run(std::chrono::seconds duration, std::uint32_t bytesPerSecond, std::chrono::milliseconds rtt, Traffic traffic = {}) {
        const auto end = now + duration;
        flow.observeRoundTrip(std::chrono::duration_cast<std::chrono::microseconds>(rtt), now);
        while (now < end) {
            while (!acks.empty() && acks.front().due <= now) {
                require(flow.acknowledge(1, acks.front().frame, acks.front().decodeMicroseconds, acks.front().queueDepth, now, false, 128), "Link ACK rejected");
                minimumBitrate = std::min(minimumBitrate, flow.rate().bitrate);
                acks.pop_front();
            }
            if (now >= encodeAt && flow.canEncode(now)) {
                const auto rate = flow.rate();
                const auto bytes = traffic.fixedBytes ? traffic.fixedBytes : std::max(100u, rate.bitrate / (8 * rate.framesPerSecond));
                flow.observeFrame(traffic.type, bytes, traffic.changedPixels, 5ms);
                flow.sent(1, ++frame, now, bytes, traffic.type);
                if (traffic.type == FrameType::kVideo) {
                    videoTimes.push_back(now);
                }
                wireFree = std::max(wireFree, now) + std::chrono::microseconds(static_cast<std::uint64_t>(bytes) * 1000000 / bytesPerSecond);
                const auto delay = traffic.delayedFrames ? traffic.feedbackDelay : 0us;
                if (traffic.delayedFrames) {
                    --traffic.delayedFrames;
                }
                const auto due = wireFree + rtt + std::chrono::microseconds(traffic.decodeMicroseconds) + delay;
                acks.push_back({frame, acks.empty() ? due : std::max(due, acks.back().due), traffic.decodeMicroseconds, traffic.queueDepth});
                encodeAt = now + std::chrono::microseconds(1000000 / rate.framesPerSecond);
                peakFrames = std::max(peakFrames, flow.pendingFrames());
                peakBytes = std::max(peakBytes, flow.pendingBytes());
            }
            now += 1ms;
        }
    }
};

void unsaturatedVideoJitter() {
    Link link;
    link.flow.setFixedPictureCost(false);
    link.run(5s, 10000000, 100ms, {.fixedBytes = 5000, .changedPixels = 128 * 64});
    for (unsigned cycle = 0; cycle < 12; ++cycle) {
        link.run(1s, 10000000, 100ms, {.fixedBytes = 5000, .changedPixels = 128 * 64, .delayedFrames = 3, .feedbackDelay = 120ms});
    }
    std::cout << "unsaturated_video_jitter minimum_bitrate=" << link.minimumBitrate << " fps=" << link.flow.rate().framesPerSecond << '\n';
    require(link.minimumBitrate == DesktopFrameFlow::kTargetVideoBitrate, "Unsaturated video display jitter collapsed the clarity budget");
}

void twoHopAsymmetricRttWindowDrag() {
    // Native ping RTT only sees Agent<->Server (80ms), while browser ACKs cross
    // Agent<->Server<->Browser (180ms). A window drag after idle must not mistake
    // the second hop's unloaded 100ms RTT for persistent congestion or trap FPS.
    DesktopFrameFlow flow;
    flow.setFixedPictureCost(false);
    auto now = DesktopFrameFlow::Clock::time_point{1s};
    flow.observeRoundTrip(80ms, now);
    std::uint64_t id = 0;
    flow.sent(1, ++id, now, 12000, FrameType::kKeyFrame);
    require(flow.acknowledge(1, id, 2000, 0, now + 182ms, false, 128), "Two-hop baseline ACK rejected");
    now += 200ms;
    for (unsigned i = 0; i < 6; ++i) {
        flow.sent(1, ++id, now, 800, FrameType::kRepeatedVideo);
        require(flow.acknowledge(1, id, 1000, 0, now + 181ms, false, 128), "Two-hop idle repeat ACK rejected");
        now += 200ms;
    }
    require(flow.snapshot(now).pathDelayMicroseconds >= 175000, "Two-hop unloaded transit did not raise path delay above one-hop ping RTT");
    std::deque<std::pair<std::uint64_t, DesktopFrameFlow::Clock::time_point>> acks;
    for (unsigned step = 0; step < 4000; ++step) {
        while (!acks.empty() && acks.front().second <= now) {
            require(flow.acknowledge(1, acks.front().first, 1500, 0, now, false, 128), "Two-hop drag ACK rejected");
            acks.pop_front();
        }
        const auto period = std::chrono::microseconds(1000000 / flow.rate().framesPerSecond);
        if ((acks.empty() || now - (acks.back().second - 184ms) >= period) && flow.canEncode(now)) {
            flow.observeFrame(FrameType::kVideo, 18000, 640 * 480, 12ms);
            flow.sent(1, ++id, now, 18000, FrameType::kVideo);
            acks.push_back({id, now + 184ms});
        }
        now += 1ms;
    }
    require(flow.rate().bitrate == DesktopFrameFlow::kTargetVideoBitrate && flow.rate().framesPerSecond >= 50,
            "Two-hop browser RTT falsely triggered congestion backoff during window drag");
}

void normalVbvAfterBurst() {
    // V2 VBV pictures: one oversized changed picture is bounded by byte credit,
    // not by seconds of reduced cadence while the +1 FPS probe revalidates.
    DesktopFrameFlow vbv;
    vbv.setFixedPictureCost(false);
    vbv.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
    require(vbv.rate().framesPerSecond >= 30 && vbv.byteWindow() == 800000, "A single VBV burst reduced cadence or escaped its credit cushion");
    for (unsigned sample = 0; sample < 12; ++sample) {
        const auto rate = vbv.rate();
        vbv.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond), 1920 * 1080, 5ms);
    }
    require(vbv.rate().framesPerSecond == DesktopFrameFlow::kMaximumFramesPerSecond, "Healthy VBV motion did not reach the adaptive cadence ceiling");
    for (const auto overhead : {0u, 256u}) {
        DesktopFrameFlow flow;
        flow.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
        const auto reduced = flow.rate().framesPerSecond;
        unsigned firstRecovered = 0;
        for (unsigned sample = 1; sample <= 100; ++sample) {
            const auto rate = flow.rate();
            flow.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond) + overhead, 1920 * 1080, 5ms);
            require(flow.rate().framesPerSecond <= rate.framesPerSecond + 1, "VBV cost revalidation jumped more than one FPS");
            if (!firstRecovered && flow.rate().framesPerSecond >= 30) {
                firstRecovered = sample;
            }
        }
        std::cout << "fixed_cost_after_single_burst initial_fps=" << reduced << " frames=100 overhead_bytes=" << overhead << " ending_fps=" << flow.rate().framesPerSecond << " first_30fps_sample=" << firstRecovered << '\n';
        require(reduced == DesktopFrameFlow::kMinimumFramesPerSecond && flow.rate().framesPerSecond >= 25 && firstRecovered == 0 || (reduced == DesktopFrameFlow::kMinimumFramesPerSecond && flow.rate().framesPerSecond >= 30 && firstRecovered), "A single fixed-cost burst permanently locked cadence without an artificial cheap frame");
    }
    DesktopFrameFlow cpu;
    cpu.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 40ms);
    for (unsigned sample = 0; sample < 100; ++sample) {
        const auto rate = cpu.rate();
        cpu.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond), 1920 * 1080, 40ms);
    }
    require(cpu.rate().framesPerSecond == 20, "Payload revalidation bypassed sustained encoder pressure");
}

void fixedSharpCapacityChange() {
    Link link;
    link.run(8s, 10000000, 40ms, {.fixedBytes = 100000});
    const auto fast = link.videoTimes.size();
    link.run(30s, 125000, 40ms, {.fixedBytes = 100000});
    const auto narrow = link.videoTimes.size() - fast;
    const auto narrowRate = link.flow.rate();
    std::cout << "fixed_sharp_narrow capacity_bps=1000000 frames_30s=" << narrow << " bitrate=" << narrowRate.bitrate << " fps=" << narrowRate.framesPerSecond
              << " peak_frames=" << link.peakFrames << " peak_bytes=" << link.peakBytes << '\n';
    require(narrow <= 45 && narrowRate.bitrate < 2000000 && narrowRate.framesPerSecond == 5, "Actual fixed sharp frames ignored a genuine capacity collapse");
    require(link.peakFrames <= 128 && link.peakBytes <= 4 * 1024 * 1024 + 100000, "Fixed sharp capacity collapse escaped existing credit bounds");
    const auto beforeRecovery = link.videoTimes.size();
    link.run(30s, 10000000, 40ms, {.fixedBytes = 100000});
    std::cout << "fixed_sharp_recovery frames_30s=" << link.videoTimes.size() - beforeRecovery << " bitrate=" << link.flow.rate().bitrate << " fps=" << link.flow.rate().framesPerSecond << '\n';
    require(link.flow.rate().bitrate == ultraterimal::agent::DesktopFrameFlow::kTargetVideoBitrate && link.flow.rate().framesPerSecond >= 19 && link.flow.rate().framesPerSecond <= 21, "Stale delivery/frame-cost history prevented capacity recovery");
    link.run(20s, 125000, 40ms, {.fixedBytes = 100000});
    require(link.flow.rate().bitrate < 2000000 && link.flow.rate().framesPerSecond == 5, "Restoration hysteresis ignored renewed fixed-sharp congestion");
    Link distant;
    distant.run(12s, 10000000, 600ms, {.fixedBytes = 100000});
    require(distant.videoTimes.size() > 200 && distant.flow.rate().bitrate == ultraterimal::agent::DesktopFrameFlow::kTargetVideoBitrate && distant.flow.rate().framesPerSecond >= 19, "High RTT alone throttled fixed sharp pictures");
}

void resolutionRecommendedBitrates() {
    require(DesktopFrameFlow::targetBitrateForGeometry(1280, 720) == 8000000u, "720p target bitrate mismatch");
    require(DesktopFrameFlow::targetBitrateForGeometry(1920, 1080) == 16000000u, "1080p target bitrate mismatch");
    require(DesktopFrameFlow::targetBitrateForGeometry(2560, 1440) == 24000000u, "1440p target bitrate mismatch");
    require(DesktopFrameFlow::targetBitrateForGeometry(3840, 2160) == 32000000u, "4K target bitrate mismatch");
    DesktopFrameFlow flow;
    require(flow.setGeometry(1280, 720) && flow.rate().bitrate == 8000000u, "setGeometry(720p) did not adopt 8 Mbps target");
    require(flow.setGeometry(2560, 1440) && flow.rate().bitrate == 24000000u, "setGeometry(1440p) did not adopt 24 Mbps target");
    require(flow.setGeometry(3840, 2160) && flow.rate().bitrate == 32000000u, "setGeometry(4K) did not adopt 32 Mbps target");
    require(flow.setGeometry(1920, 1080) && flow.rate().bitrate == 16000000u, "setGeometry(1080p) did not clamp back to 16 Mbps");
}

void weakNetworkSevereStallBackoff() {
    // A single severe saturated stall past maximumFrameAge (e.g. packet-loss
    // recovery on a weak link) must immediately reduce bitrate instead of
    // waiting for three separate stalls.
    DesktopFrameFlow flow;
    flow.setFixedPictureCost(false);
    auto now = DesktopFrameFlow::Clock::time_point{1s};
    flow.observeRoundTrip(20ms, now);
    flow.sent(1, 1, now, 12000, FrameType::kKeyFrame);
    require(flow.acknowledge(1, 1, 1000, 0, now + 22ms, false, 128), "Severe stall baseline ACK failed");
    now += 100ms;
    flow.sent(1, 2, now, 80000, FrameType::kVideo);
    flow.sent(1, 3, now + 16ms, 80000, FrameType::kVideo);
    const auto before = flow.rate().bitrate;
    require(flow.acknowledge(1, 3, 1000, 0, now + 600ms, false, 128), "Severe stall ACK failed");
    require(flow.rate().bitrate <= before * 2 / 3, "Severe saturated stall did not trigger immediate bitrate backoff");
}

} // namespace
int main() {
    try {
        unsaturatedVideoJitter();
        twoHopAsymmetricRttWindowDrag();
        normalVbvAfterBurst();
        fixedSharpCapacityChange();
        resolutionRecommendedBitrates();
        weakNetworkSevereStallBackoff();
        auto now = DesktopFrameFlow::Clock::time_point{1s};
        DesktopFrameFlow flow;
        DesktopFrameFlow motionCredit;
        motionCredit.observeFrame(FrameType::kVideo, 1000, 128 * 64, 40ms);
        motionCredit.observeRoundTrip(10ms, now);
        motionCredit.sent(1, 1, now, 1000, FrameType::kKeyFrame);
        require(motionCredit.acknowledge(1, 1, 1000, 0, now + 10ms), "Motion credit baseline ACK failed");
        motionCredit.observeFrame(FrameType::kVideo, 160000, 128 * 64, 40ms);
        motionCredit.sent(1, 2, now + 33ms, 160000, FrameType::kVideo);
        require(motionCredit.canEncode(now + 34ms), "A normal motion frame exhausted all pipeline credit");
        motionCredit.observeFrame(FrameType::kVideo, 160000, 128 * 64, 40ms);
        motionCredit.sent(1, 3, now + 66ms, 160000, FrameType::kVideo);
        require(!motionCredit.canEncode(now + 67ms) && motionCredit.pendingBytes() == 320000, "Motion allowance escaped its bounded two-frame cushion");
        require(!motionCredit.canEncode(now + 230ms), "Motion cushion disabled the oldest-frame guard");
        require(motionCredit.acknowledge(1, 2, 1000, 0, now + 70ms) && motionCredit.canEncode(now + 70ms), "Motion ACK failed to reopen bounded pipeline credit");
        DesktopFrameFlow continuous;
        continuous.sent(1, 1, now, 1000, FrameType::kKeyFrame);
        require(continuous.acknowledge(1, 1, 350000, 0, now + 10ms), "Cold keyframe ACK failed");
        require(continuous.rate().framesPerSecond == 30, "Cold decoder startup poisoned steady cadence");
        DesktopFrameFlow idleCapacity;
        idleCapacity.observeRoundTrip(80ms, now);
        idleCapacity.sent(1, 1, now, 1000, FrameType::kKeyFrame);
        idleCapacity.sent(1, 2, now + 33333us, 1000, FrameType::kRepeatedVideo);
        idleCapacity.sent(1, 3, now + 66666us, 1000, FrameType::kRepeatedVideo);
        require(idleCapacity.acknowledge(1, 1, 1000, 0, now + 80ms), "Idle capacity baseline ACK failed");
        const auto capacityBeforeIdle = idleCapacity.byteWindow();
        for (std::uint64_t frame = 2; frame <= 36; ++frame) {
            idleCapacity.sent(1, frame + 2, now + 33333us * (frame + 1), 1000, FrameType::kRepeatedVideo);
            require(idleCapacity.acknowledge(1, frame, 1000, 0, now + 33333us * (frame - 1) + 80ms), "Continuous idle ACK failed");
        }
        require(idleCapacity.byteWindow() == capacityBeforeIdle && idleCapacity.rate().framesPerSecond == 30, "Application-limited idle repeats collapsed credit before a window drag");
        idleCapacity.sent(1, 39, now + 1300ms, 50000, FrameType::kVideo);
        require(idleCapacity.canEncode(now + 1301ms), "Idle-to-drag transition was backpressured by idle throughput");
        continuous.sent(1, 2, now + 20ms, 100, FrameType::kRepeatedVideo);
        require(continuous.acknowledge(1, 2, 1000, 0, now + 30ms), "Real repeated frame did not return credit");
        continuous.sent(1, 3, now + 110ms, 1000, FrameType::kVideo);
        require(flow.rate().framesPerSecond == 30 && flow.rate().bitrate == ultraterimal::agent::DesktopFrameFlow::kTargetVideoBitrate, "Desktop did not default to the fixed sharp rate");
        flow.sent(1, 1, now, 4000000, FrameType::kKeyFrame);
        require(!flow.canEncode(now + 1ms), "Oversized IDR did not exhaust byte credit");
        const auto blocked = flow.snapshot(now + 1ms);
        require(blocked.pendingFrames == 1 && blocked.pendingBytes == 4000000 && blocked.pressureBytes == 4000000 && (blocked.blockingFlags & 4) && blocked.oldestFrameAgeMicroseconds == 1000,
                "Flow sampling lost the byte-credit stall and oldest frame age");
        require(!flow.acknowledge(1, 2, 1000, 0, now + 20ms, false, 128), "Future ACK accepted");
        require(!flow.acknowledge(2, 1, 1000, 0, now + 20ms, false, 128), "Wrong-generation ACK accepted");
        require(!flow.acknowledge(1, 1, 1000, 0, now + 20ms, false, 129), "Invalid receiver capacity accepted");
        require(flow.acknowledge(1, 1, 1000, 0, now + 20ms, false, 1) && flow.frameWindow() == 1, "Receiver capacity ignored");
        const auto bitrate = flow.rate().bitrate;
        require(flow.acknowledge(1, 1, 600000, 128, now + 1s, true, 1) && !flow.canEncode(now + 1s), "Repeated ACK did not pause a static desktop");
        require(flow.rate().bitrate == bitrate, "Repeated ACK changed capacity estimate");
        require(flow.acknowledge(1, 1, 1000, 0, now + 2s, false, 128) && flow.canEncode(now + 2s), "Visible static desktop did not resume");
        flow.sent(1, 2, now + 2s, 1000, FrameType::kVideo);
        flow.sent(1, 3, now + 2s, 1000, FrameType::kVideo);
        require(flow.acknowledge(1, 3, 1000, 0, now + 2030ms, false, 128) && flow.pendingBytes() == 0, "Cumulative ACK leaked credit");
        flow.sent(2, 1, now + 3s, 1000, FrameType::kVideo);
        require(!flow.acknowledge(1, 3, 1000, 0, now + 4s), "Retired generation affected flow");
        require(flow.acknowledge(2, 1, 1000, 0, now + 4s), "New baseline ACK failed");
        require(!flow.setGeometry(4096, 4096) && !flow.setGeometry(17, 16), "Invalid geometry accepted");
        DesktopFrameFlow cpu;
        cpu.observeFrame(FrameType::kVideo, 1000, 1920 * 1080, 40ms);
        require(cpu.rate().framesPerSecond <= 20, "Encoding pressure did not reduce capture load");
        DesktopFrameFlow motion;
        motion.observeFrame(FrameType::kKeyFrame, 300000, 1920 * 1080, 350ms);
        require(motion.rate().framesPerSecond == 30, "Codec startup reduced steady motion cadence");
        for (unsigned sample = 0; sample < 24; ++sample) {
            const auto rate = motion.rate();
            motion.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond), 1920 * 1080, 12ms);
        }
        require(motion.rate().framesPerSecond == DesktopFrameFlow::kMaximumFramesPerSecond, "Rate-controlled motion did not reach the adaptive ceiling on an uncongested path");
        DesktopFrameFlow decoder;
        decoder.observeRoundTrip(20ms, now);
        for (std::uint64_t id = 1; id <= 4; ++id) {
            now += 100ms;
            decoder.sent(1, id, now, 1000, FrameType::kVideo);
            require(decoder.acknowledge(1, id, 1000, 3, now + 21ms), "Transient decoder queue ACK rejected");
        }
        require(decoder.rate().framesPerSecond == 30, "Transient queue depth within 4-frame decode pipeline reduced FPS");
        for (std::uint64_t id = 5; id <= 16; ++id) {
            now += 300ms;
            decoder.sent(1, id, now, 1000, FrameType::kVideo);
            require(decoder.acknowledge(1, id, 1000, 4, now + 21ms), "Decoder backlog ACK rejected");
        }
        require(decoder.rate().framesPerSecond < 30 && decoder.rate().bitrate == ultraterimal::agent::DesktopFrameFlow::kTargetVideoBitrate, "Decoder backlog did not reduce capture independently of network bitrate");
        DesktopFrameFlow idle;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            now += 2s;
            idle.sent(1, id, now, 1000, FrameType::kVideo);
            require(idle.acknowledge(1, id, 1000, 0, now + 20ms), "Idle ACK rejected");
        }
        require(idle.rate().framesPerSecond >= 30 && idle.rate().bitrate == ultraterimal::agent::DesktopFrameFlow::kTargetVideoBitrate, "Static idle time was interpreted as narrow capacity");
        Link fast;
        fast.run(12s, 10000000, 20ms);
        std::cout << "fast_12s frames=" << fast.frame << " fps=" << fast.flow.rate().framesPerSecond << '\n';
        require(fast.frame > 660 && fast.frame <= 720 && fast.flow.rate().framesPerSecond == DesktopFrameFlow::kMaximumFramesPerSecond, "Fast path failed to sustain the adaptive FPS ceiling");
        Link distant;
        distant.run(12s, 10000000, 600ms);
        std::cout << "distant_12s frames=" << distant.frame << " fps=" << distant.flow.rate().framesPerSecond << '\n';
        require(distant.frame > 600 && distant.frame <= 720 && distant.flow.rate().framesPerSecond == DesktopFrameFlow::kMaximumFramesPerSecond, "Stable high RTT caused unnecessary rate reduction");
        Link narrow;
        narrow.run(30s, 125000, 40ms);
        require(narrow.flow.rate().bitrate < 2000000 && narrow.flow.rate().framesPerSecond < 30, "Narrow link failed to adapt both bitrate and capture cadence");
        require(narrow.peakFrames <= 128 && narrow.peakBytes <= 4 * 1024 * 1024 + 50000, "Narrow link exceeded bounded flight");
        const auto reduced = narrow.flow.rate();
        narrow.run(25s, 10000000, 40ms);
        std::cout << "recovery_25s_bitrate=" << narrow.flow.rate().bitrate << " fps=" << narrow.flow.rate().framesPerSecond << '\n';
        require(narrow.flow.rate().bitrate > reduced.bitrate * 8 && narrow.flow.rate().framesPerSecond == DesktopFrameFlow::kMaximumFramesPerSecond, "Capacity recovery failed to restore the adaptive FPS ceiling");
        narrow.run(30s, 125000, 40ms);
        require(narrow.flow.rate().bitrate < 2000000 && narrow.flow.rate().framesPerSecond < 30, "Recovery ignored renewed congestion");
        DesktopFrameFlow stalled;
        stalled.sent(1, 1, now, 1000, FrameType::kVideo);
        require(!stalled.canEncode(now + 1s), "Missing ACK did not stop further encoding");
        require(stalled.acknowledge(1, 1, 1000, 0, now + 2s) && stalled.canEncode(now + 2s), "ACK did not resume a stalled window");
        std::cout << "Receiver credit, finite-capacity congestion/recovery, high RTT, idle, CPU pressure and generation safety passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
