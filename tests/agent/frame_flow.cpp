#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>

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
                flow.observeFrame(FrameType::kVideo, bytes, 1920 * 1080, 5ms);
                flow.sent(1, ++frame, now, bytes, FrameType::kVideo);
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
        require(flow.rate().framesPerSecond == 30, "Healthy desktop exceeded the configured 30 FPS ceiling");
        flow.sent(1, 1, now, 4000000, FrameType::kVideo);
        require(!flow.canEncode(now + 1ms), "Oversized IDR did not exhaust byte credit");
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
        motion.setQuality(DesktopFrameFlow::Quality::kSmooth);
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
