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
void smallRegionToSharpMotion() {
    Link link;
    bool healthyTransitions = true;
    for (unsigned cycle = 0; cycle < 4; ++cycle) {
        link.run(3s, 10000000, 80ms, {.type = FrameType::kRegion, .fixedBytes = 1000, .changedPixels = 128 * 64});
        const auto lowLoad = link.flow.snapshot(link.now).deliveryBytesPerSecond;
        link.run(1s, 10000000, 80ms, {.type = FrameType::kRegion, .fixedBytes = 1000, .changedPixels = 128 * 64, .delayedFrames = 3, .feedbackDelay = 80ms});
        const auto afterDelay = link.flow.rate();
        const auto firstVideo = link.videoTimes.size();
        link.run(4s, 10000000, 80ms, {.fixedBytes = 100000});
        std::vector<std::int64_t> gaps;
        for (std::size_t index = firstVideo + 1; index < link.videoTimes.size(); ++index) {
            gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(link.videoTimes[index] - link.videoTimes[index - 1]).count());
        }
        std::sort(gaps.begin(), gaps.end());
        const auto p95 = gaps.empty() ? 0 : gaps[(gaps.size() - 1) * 95 / 100];
        const auto videos = link.videoTimes.size() - firstVideo;
        std::cout << "region_to_sharp cycle=" << cycle << " capacity_bps=80000000 rtt_ms=80 capacity_estimate_Bps=" << lowLoad
                  << " after_delay_bitrate=" << afterDelay.bitrate << " video_frames_4s=" << videos << " video_gap_p95_ms=" << p95
                  << " ending_bitrate=" << link.flow.rate().bitrate << " ending_fps=" << link.flow.rate().framesPerSecond << '\n';
        healthyTransitions = healthyTransitions && videos >= 100 && p95 <= 60;
    }
    require(healthyTransitions, "An ample-capacity region-to-video transition retained a long sharp-video stall");
    require(link.minimumBitrate >= 12000000, "Unsaturated region ACKs collapsed a fixed ample-capacity sharp path");
    require(link.videoTimes.size() >= 280, "Repeated small-region/sharp-video transitions remained stuck at 3-5 FPS");
}
void normalVbvAfterBurst() {
    for (const auto overhead : {0u, 256u}) {
        DesktopFrameFlow flow;
        flow.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
        const auto reduced = flow.rate().framesPerSecond;
        unsigned firstRecovered = 0;
        for (unsigned sample = 1; sample <= 100; ++sample) {
            const auto rate = flow.rate();
            flow.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond) + overhead, 1920 * 1080, 5ms);
            require(flow.rate().framesPerSecond <= rate.framesPerSecond + 1, "VBV cost revalidation jumped more than one FPS");
            if (!firstRecovered && flow.rate().framesPerSecond == 30) {
                firstRecovered = sample;
            }
        }
        std::cout << "vbv_after_single_burst initial_fps=" << reduced << " frames=100 overhead_bytes=" << overhead << " ending_fps=" << flow.rate().framesPerSecond << " first_30fps_sample=" << firstRecovered << '\n';
        require(reduced == 7 && flow.rate().framesPerSecond == 30 && firstRecovered, "A single non-key burst permanently locked normal VBV cadence without an artificial cheap frame");
    }
    DesktopFrameFlow cpu;
    cpu.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 40ms);
    for (unsigned sample = 0; sample < 100; ++sample) {
        const auto rate = cpu.rate();
        cpu.observeFrame(FrameType::kVideo, rate.bitrate / (8 * rate.framesPerSecond), 1920 * 1080, 40ms);
    }
    require(cpu.rate().framesPerSecond == 20, "Payload revalidation bypassed sustained encoder pressure");
}
void actualFrameCost() {
    DesktopFrameFlow flow;
    flow.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
    require(flow.rate().framesPerSecond == 7 && flow.byteWindow() == 800000, "A real over-budget sharp picture did not bound the next encode cadence/credit");
    const auto window = flow.byteWindow();
    flow.observeFrame(FrameType::kKeyFrame, 4000000, 1920 * 1080, 350ms);
    require(flow.rate().framesPerSecond == 7 && flow.byteWindow() == window, "Keyframe burst poisoned steady frame cost");
    flow.observeFrame(FrameType::kRepeatedVideo, 100, 0, 5ms);
    flow.observeFrame(FrameType::kVideo, 4000000, 0, 5ms);
    require(flow.byteWindow() == window, "Repeated/static frames inflated or diluted the actual changed-picture window");
    for (unsigned sample = 0; sample < 12; ++sample) {
        flow.observeFrame(FrameType::kRegion, 1000, 128 * 64, 5ms);
    }
    require(flow.rate().framesPerSecond == 30, "Historical sharp picture cost throttled cheap regions");
    flow.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
    require(flow.rate().framesPerSecond == 7, "Region-to-sharp transition lost the real video payload floor");
    unsigned probes = 0;
    for (unsigned sample = 0; sample < 100; ++sample) {
        const auto previousFps = flow.rate().framesPerSecond;
        flow.observeFrame(FrameType::kVideo, 400000, 1920 * 1080, 5ms);
        const auto fps = flow.rate().framesPerSecond;
        require(fps == 7 || fps == 8, "Cost revalidation unconditionally released a genuinely fixed sharp payload floor");
        if (previousFps == 8) {
            require(fps == 7, "A fixed sharp frame that exceeded the probe budget did not immediately restore its real cost");
        }
        probes += fps == 8;
    }
    std::cout << "fixed_sharp_cost_revalidation samples=100 baseline_fps=7 maximum_probe_fps=8 probes=" << probes << " ending_fps=" << flow.rate().framesPerSecond << '\n';
    require(probes > 0 && flow.byteWindow() == window, "Cost revalidation was absent or widened the existing ordinary-frame credit cushion");
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
    require(link.flow.rate().bitrate == 24000000 && link.flow.rate().framesPerSecond == 30, "Stale delivery/frame-cost history prevented capacity recovery");
    link.run(20s, 125000, 40ms, {.fixedBytes = 100000});
    require(link.flow.rate().bitrate < 2000000 && link.flow.rate().framesPerSecond == 5, "Restoration hysteresis ignored renewed fixed-sharp congestion");
    Link distant;
    distant.run(12s, 10000000, 600ms, {.fixedBytes = 100000});
    require(distant.videoTimes.size() > 300 && distant.flow.rate().bitrate == 24000000 && distant.flow.rate().framesPerSecond == 30, "High RTT alone throttled fixed sharp pictures");
}
void lowLoadBackgroundAndDecode() {
    for (const auto type : {FrameType::kRegion, FrameType::kVideo}) {
        Link link;
        link.run(5s, 10000000, 80ms, {.type = type, .fixedBytes = 1000, .changedPixels = 128 * 64});
        require(!link.flow.snapshot(link.now).deliveryBytesPerSecond && link.flow.rate().bitrate == 24000000, "Unsaturated small updates created a false capacity ceiling");
        // A cumulative display ACK releases all real outstanding frames before
        // the hidden interval; no queued dependent frame is silently discarded.
        link.now += 1s;
        require(link.flow.acknowledge(1, link.frame, 600000, 128, link.now, true, 128) && !link.flow.canEncode(link.now), "Background ACK did not suspend low-load traffic");
        link.acks.clear();
        link.now += 5s;
        require(link.flow.acknowledge(1, link.frame, 1000, 0, link.now, false, 128), "Foreground duplicate ACK was rejected");
        const auto before = link.videoTimes.size();
        link.run(5s, 10000000, 80ms, {.fixedBytes = 100000});
        require(link.videoTimes.size() - before >= 140 && link.flow.rate().bitrate == 24000000, "Hidden time/decoder feedback polluted foreground sharp recovery");
    }
    Link decoder;
    decoder.run(8s, 10000000, 80ms, {.fixedBytes = 100000, .decodeMicroseconds = 60000, .queueDepth = 4});
    require(decoder.flow.rate().bitrate == 24000000 && decoder.flow.rate().framesPerSecond <= 13 && !decoder.flow.snapshot(decoder.now).deliveryBytesPerSecond,
            "Slow decoding became a false bandwidth ceiling or failed to bound capture cadence");
    const auto now = DesktopFrameFlow::Clock::time_point{1s};
    DesktopFrameFlow startup;
    startup.observeRoundTrip(20ms, now);
    startup.sent(1, 1, now, 4000000, FrameType::kKeyFrame);
    startup.sent(1, 2, now + 34ms, 100000, FrameType::kVideo);
    require(startup.acknowledge(1, 2, 1000, 0, now + 500ms) && !startup.snapshot(now + 500ms).deliveryBytesPerSecond,
            "Cumulative keyframe startup bytes were treated as ordinary path capacity");
}
} // namespace
int main() {
    try {
        normalVbvAfterBurst();
        smallRegionToSharpMotion();
        actualFrameCost();
        fixedSharpCapacityChange();
        lowLoadBackgroundAndDecode();
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
        require(continuous.sentRefinement(1, 1, 1, now + 10ms, 1000), "Continuous baseline refinement rejected");
        continuous.sent(1, 2, now + 20ms, 100, FrameType::kRepeatedVideo);
        require(continuous.acknowledge(1, 2, 1000, 0, now + 30ms), "Real repeated frame did not return credit");
        require(continuous.refinementOutstanding(now + 30ms) && !continuous.refinementRetired(now + 30ms), "Repeated picture retired static refinement");
        require(continuous.acknowledgeRefinement(1, 1, 1, now + 30ms), "Stable content refinement lost its exact ACK tuple");
        require(!continuous.sentRefinement(1, 2, 2, now + 100ms, 1000), "Delivery alias accepted as a new content baseline");
        require(continuous.sentRefinement(1, 1, 2, now + 100ms, 1000), "Refinement could not continue after duplicate delivery");
        continuous.sent(1, 3, now + 110ms, 1000, FrameType::kVideo);
        require(continuous.refinementRetired(now + 110ms), "Changed content did not retire old refinement");
        require(flow.rate().framesPerSecond == 30 && flow.rate().bitrate == 24000000, "Desktop did not default to the fixed sharp rate");
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
        DesktopFrameFlow detail;
        const auto detailNow = DesktopFrameFlow::Clock::time_point{1s};
        detail.sent(1, 1, detailNow, 1000, FrameType::kVideo);
        const auto budget = detail.refinementBudget(detailNow);
        require(budget > 0 && budget <= 65536, "Refinement budget is absent/unbounded");
        require(detail.sentRefinement(1, 1, 1, detailNow, budget), "First independent refinement rejected");
        require(detail.pendingFrames() == 1 && detail.pendingBytes() == 1000 && detail.pressureBytes(detailNow) == 1000 + budget, "Refinement became a video frame or disappeared from pressure");
        require(detail.refinementOutstanding(detailNow) && !detail.refinementBudget(detailNow), "Second refinement bypassed global outstanding credit");
        require(!detail.acknowledgeRefinement(1, 2, 1, detailNow) && !detail.acknowledgeRefinement(2, 1, 1, detailNow) && !detail.acknowledgeRefinement(1, 1, 2, detailNow), "Nonmatching ACK released refinement");
        require(detail.acknowledge(1, 1, 0, 0, detailNow + 1ms), "Video ACK was blocked by refinement");
        require(detail.refinementOutstanding(detailNow + 1ms), "Ordinary frame ACK released refinement credit");
        require(detail.acknowledgeRefinement(1, 1, 1, detailNow + 1ms), "Exact refinement tuple did not release credit");
        require(detail.pressureBytes(detailNow + 1ms) > 0, "Refinement ACK refunded written bytes");
        require(!detail.acknowledgeRefinement(1, 1, 1, detailNow + 1ms), "Duplicate refinement ACK affected credit");
        require(detail.refinementBudget(detailNow + 100ms) > 0, "Timed throughput tokens did not refill");
        require(!detail.sentRefinement(1, 1, 1, detailNow + 100ms, 100), "Batch ID reused within generation");
        require(detail.sentRefinement(1, 1, 3, detailNow + 100ms, 1000), "Skipped monotonic batch ID rejected");
        detail.retireRefinement(detailNow + 100ms);
        const auto retiredPressure = detail.pressureBytes(detailNow + 100ms);
        require(retiredPressure > 0 && detail.refinementRetired(detailNow + 100ms) && detail.refinementOutstanding(detailNow + 100ms), "Retirement refunded bytes or released unacked resource ownership");
        require(!detail.sentRefinement(1, 1, 4, detailNow + 100ms, 100) && !detail.refinementBudget(detailNow + 100ms), "Retired decode allowed a second unacked batch");
        detail.sent(1, 2, detailNow + 100ms, 1000, FrameType::kVideo);
        require(detail.acknowledge(1, 2, 0, 0, detailNow + 100ms) && detail.refinementOutstanding(detailNow + 100ms), "New frame/ordinary ACK released retired refinement credit");
        require(detail.acknowledgeRefinement(1, 1, 3, detailNow + 100ms) && detail.pressureBytes(detailNow + 100ms) == retiredPressure, "Retired tuple cleanup failed or refunded written pressure");
        require(detail.sentRefinement(1, 2, 4, detailNow + 101ms, 1000), "Resource cleanup did not release independent credit");
        require(!detail.acknowledgeRefinement(1, 1, 3, detailNow + 101ms) && detail.refinementOutstanding(detailNow + 101ms), "Late retired ACK released a newer batch's credit");
        const auto beforeBarrier = detail.pressureBytes(detailNow + 101ms);
        detail.sent(2, 1, detailNow + 101ms, 1000, FrameType::kVideo);
        require(detail.refinementRetired(detailNow + 101ms) && detail.refinementOutstanding(detailNow + 101ms) && !detail.refinementBudget(detailNow + 101ms), "Generation barrier refunded unacked credit");
        require(detail.pressureBytes(detailNow + 101ms) == beforeBarrier + 1000, "Generation barrier erased refinement byte pressure");
        require(!detail.sentRefinement(2, 1, 1, detailNow + 101ms, 100), "Generation barrier bypassed the single native decode lease");
        require(detail.acknowledgeRefinement(1, 2, 4, detailNow + 101ms), "Cross-generation cleanup ACK could not release its exact old tuple");
        require(detail.sentRefinement(2, 1, 1, detailNow + 101ms, 1000), "Clean new generation could not restart batch IDs");
        require(!detail.acknowledgeRefinement(1, 2, 4, detailNow + 101ms) && detail.refinementOutstanding(detailNow + 101ms), "Old generation duplicate released a new tuple with reused batch ID");
        detail.sent(2, 2, detailNow + 102ms, 1000, FrameType::kVideo);
        require(detail.acknowledge(2, 2, 0, 0, detailNow + 103ms) && detail.refinementOutstanding(detailNow + 103ms), "New video failed to progress independently");
        require(detail.refinementPaused(detailNow + 2101ms) && detail.refinementOutstanding(detailNow + 2101ms) && !detail.refinementBudget(detailNow + 2101ms), "Hung PNG timeout dropped the native resource lease or failed to pause detail");
        require(detail.canEncode(detailNow + 2101ms), "Hung PNG blocked independent video progress");
        detail.sent(3, 1, detailNow + 2101ms, 1000, FrameType::kVideo);
        require(!detail.refinementBudget(detailNow + 2101ms) && !detail.sentRefinement(3, 1, 1, detailNow + 2101ms, 100), "New generation restarted optional work while timed-out native decode remained alive");
        require(detail.acknowledgeRefinement(2, 1, 1, detailNow + 2102ms) && !detail.refinementOutstanding(detailNow + 2102ms) && !detail.refinementPaused(detailNow + 2102ms), "Old timeout cleanup paused the new generation");
        require(detail.sentRefinement(3, 1, 1, detailNow + 2102ms, 1000), "Old timeout cleanup did not allow current-generation refinement");
        require(!detail.acknowledgeRefinement(2, 1, 1, detailNow + 2102ms) && detail.refinementOutstanding(detailNow + 2102ms), "Exact old ACK released the current-generation tuple");
        require(detail.acknowledgeRefinement(3, 1, 1, detailNow + 2102ms), "Current-generation cleanup failed");
        detail.sent(4, 1, detailNow + 2103ms, 1000, FrameType::kVideo);
        require(!detail.refinementPaused(detailNow + 2103ms) && detail.sentRefinement(4, 1, 1, detailNow + 2103ms, 1000), "Post-cleanup new generation did not resume optional work");
        require(!detail.acknowledgeRefinement(2, 1, 1, detailNow + 2103ms) && detail.pendingFrames() == 1 && detail.refinementOutstanding(detailNow + 2103ms), "Late cross-generation ACK affected new video/refinement credit");
        // Cleanup itself may be the first update at/after the old deadline.
        for (const auto cleanupDelay : {1999ms, 2000ms, 2100ms}) {
            DesktopFrameFlow crossed;
            crossed.sent(1, 1, detailNow, 1000, FrameType::kVideo);
            require(crossed.sentRefinement(1, 1, 1, detailNow, 1000), "Cross-generation timeout setup failed");
            crossed.reset(2);
            crossed.sent(2, 1, detailNow + 1s, 1000, FrameType::kVideo);
            require(crossed.acknowledge(2, 1, 0, 0, detailNow + 1001ms), "Cross-generation video ACK failed");
            require(crossed.refinementOutstanding(detailNow + 1001ms) && !crossed.refinementBudget(detailNow + 1001ms), "Reset released the old native lease before cleanup");
            const auto cleanupAt = detailNow + cleanupDelay;
            require(crossed.acknowledgeRefinement(1, 1, 1, cleanupAt) && !crossed.refinementPaused(cleanupAt), "Old cleanup at the timeout boundary paused the current generation");
            require(crossed.sentRefinement(2, 1, 1, cleanupAt, 1000), "Current generation could not refine after old cleanup");
            require(!crossed.acknowledgeRefinement(1, 1, 1, cleanupAt) && crossed.refinementOutstanding(cleanupAt), "Old ACK cleared a new tuple with identical frame and batch IDs");
            require(crossed.acknowledgeRefinement(2, 1, 1, cleanupAt), "Exact new tuple cleanup failed");
        }
        DesktopFrameFlow sameGeneration;
        sameGeneration.sent(1, 1, detailNow, 1000, FrameType::kVideo);
        require(sameGeneration.sentRefinement(1, 1, 1, detailNow, 1000), "Same-generation timeout setup failed");
        require(!sameGeneration.refinementPaused(detailNow + 1999ms), "Current generation paused before the timeout");
        require(sameGeneration.refinementPaused(detailNow + 2s) && sameGeneration.refinementOutstanding(detailNow + 2s), "Current-generation timeout did not pause and retain the lease");
        require(sameGeneration.acknowledgeRefinement(1, 1, 1, detailNow + 2100ms) && sameGeneration.refinementPaused(detailNow + 2100ms) && !sameGeneration.refinementBudget(detailNow + 2100ms), "Same-generation timeout cleanup resumed optional work");
        require(!sameGeneration.sentRefinement(1, 1, 2, detailNow + 2100ms, 100), "Timed-out current generation accepted another refinement");
        DesktopFrameFlow unacknowledged;
        unacknowledged.sent(1, 1, detailNow, 1000, FrameType::kVideo);
        require(unacknowledged.sentRefinement(1, 1, 1, detailNow, 1000), "Unacknowledged lease setup failed");
        unacknowledged.sent(2, 1, detailNow + 1s, 1000, FrameType::kVideo);
        require(unacknowledged.acknowledge(2, 1, 0, 0, detailNow + 1001ms), "Initial new-generation video ACK failed");
        for (std::uint64_t frame = 2; frame <= 4; ++frame) {
            const auto videoAt = detailNow + 2100ms + std::chrono::seconds(frame - 2);
            require(unacknowledged.refinementOutstanding(videoAt) && unacknowledged.refinementRetired(videoAt) && !unacknowledged.refinementPaused(videoAt) && !unacknowledged.refinementBudget(videoAt), "Old timed-out lease was freed or paused the current generation without cleanup");
            require(!unacknowledged.sentRefinement(2, frame - 1, 1, videoAt, 100), "Missing old cleanup allowed a second native lease");
            require(unacknowledged.canEncode(videoAt), "Old unacknowledged timeout stopped independent video");
            unacknowledged.sent(2, frame, videoAt, 1000, FrameType::kVideo);
            require(unacknowledged.acknowledge(2, frame, 0, 0, videoAt + 1ms), "Video could not continue without old refinement cleanup");
        }
        DesktopFrameFlow spent;
        spent.sent(1, 1, detailNow, 1000, FrameType::kVideo);
        const auto spentBudget = spent.refinementBudget(detailNow);
        require(spent.sentRefinement(1, 1, 1, detailNow, spentBudget), "Spent-token setup failed");
        spent.reset(2);
        spent.sent(2, 1, detailNow, 1000, FrameType::kVideo);
        require(spent.pressureBytes(detailNow) == 1000 + spentBudget, "Generation reset refunded written refinement bytes");
        require(spent.acknowledgeRefinement(1, 1, 1, detailNow) && !spent.refinementOutstanding(detailNow) && !spent.refinementBudget(detailNow), "Cross-generation cleanup refunded spent tokens");
        require(spent.pressureBytes(detailNow) == 1000 + spentBudget, "Cross-generation cleanup refunded written refinement bytes");
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
        require(motion.rate().framesPerSecond == 30, "Rate-controlled motion reduced its own cadence on an uncongested path");
        DesktopFrameFlow decoder;
        decoder.observeRoundTrip(20ms, now);
        for (std::uint64_t id = 1; id <= 12; ++id) {
            now += 300ms;
            decoder.sent(1, id, now, 1000, FrameType::kVideo);
            require(decoder.acknowledge(1, id, 1000, 4, now + 21ms), "Decoder backlog ACK rejected");
        }
        require(decoder.rate().framesPerSecond < 30 && decoder.rate().bitrate == 24000000, "Decoder backlog did not reduce capture independently of network bitrate");
        DesktopFrameFlow idle;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            now += 2s;
            idle.sent(1, id, now, 1000, FrameType::kVideo);
            require(idle.acknowledge(1, id, 1000, 0, now + 20ms), "Idle ACK rejected");
        }
        require(idle.rate().framesPerSecond == 30 && idle.rate().bitrate == 24000000, "Static idle time was interpreted as narrow capacity");
        Link fast;
        fast.run(12s, 10000000, 20ms);
        require(fast.frame > 330 && fast.frame <= 360 && fast.flow.rate().framesPerSecond == 30, "Fast path failed to sustain the 30 FPS ceiling");
        Link distant;
        distant.run(12s, 10000000, 600ms);
        require(distant.frame > 300 && distant.frame <= 360 && distant.flow.rate().framesPerSecond == 30, "Stable high RTT caused unnecessary rate reduction");
        Link narrow;
        narrow.run(30s, 125000, 40ms);
        require(narrow.flow.rate().bitrate < 2000000 && narrow.flow.rate().framesPerSecond < 30, "Narrow link failed to adapt both bitrate and capture cadence");
        require(narrow.peakFrames <= 128 && narrow.peakBytes <= 4 * 1024 * 1024 + 50000, "Narrow link exceeded bounded flight");
        auto scene = narrow.flow;
        const auto narrowBitrate = scene.rate().bitrate;
        for (unsigned sample = 0; sample < 24; ++sample) {
            scene.observeFrame(FrameType::kRegion, 100, 128 * 64, 5ms);
        }
        require(scene.rate().framesPerSecond > narrow.flow.rate().framesPerSecond && scene.rate().bitrate == narrowBitrate, "Cheap regional updates did not recover cadence within the same bandwidth");
        for (unsigned sample = 0; sample < 24; ++sample) {
            scene.observeFrame(FrameType::kRegion, 100000, 1920 * 1080, 5ms);
        }
        require(scene.rate().framesPerSecond <= narrow.flow.rate().framesPerSecond, "Large lossless regions ignored the actual byte budget");
        const auto sceneRate = scene.rate().framesPerSecond;
        scene.observeFrame(FrameType::kKeyFrame, 1, 1, 350ms);
        require(scene.rate().framesPerSecond == sceneRate, "Keyframe distorted the steady scene estimate");
        const auto reduced = narrow.flow.rate();
        narrow.run(25s, 10000000, 40ms);
        std::cout << "recovery_25s_bitrate=" << narrow.flow.rate().bitrate << " fps=" << narrow.flow.rate().framesPerSecond << '\n';
        require(narrow.flow.rate().bitrate > reduced.bitrate * 8 && narrow.flow.rate().framesPerSecond == 30, "Capacity recovery failed to restore the 30 FPS ceiling");
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
