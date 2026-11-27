#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "features/desktop/desktop_diagnostics.types.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Desktop encoding statistics differ");
    }
}
void stagePresence() {
    using namespace ultraterimal;
    using namespace server;
    DesktopPipelineDiagnostics diagnostics;
    wire::DesktopStageTiming timing;
    timing.set_worker_queue_microseconds(0);
    timing.set_acquire_wait_microseconds(0);
    timing.set_source_age_microseconds(0);
    timing.set_pacing_credit_wait_microseconds(0);
    timing.set_gpu_sync_wait_microseconds(0);
    timing.set_readback_ready_microseconds(0);
    timing.set_prepare_cpu_microseconds(0);
    timing.set_video_encode_microseconds(0);
    diagnostics.observe(timing);
    diagnostics.observe(wire::DesktopStageTiming{});
    for (std::size_t index = 0; index <= static_cast<std::size_t>(DesktopStage::kVideoEncode); ++index) {
        const auto summary = diagnostics.stages[index].summary();
        require(summary.samples == 1 && summary.meanMicroseconds == 0 && summary.maximumMicroseconds == 0);
    }
    require(kDesktopStageNames[static_cast<std::size_t>(DesktopStage::kPrepareCpu)] == "prepare_cpu");
    require(kDesktopStageNames[static_cast<std::size_t>(DesktopStage::kGpuSyncWait)] == "gpu_sync_wait");
    require(kDesktopStageNames[static_cast<std::size_t>(DesktopStage::kReadbackReady)] == "readback_ready");
}
void pipeline() {
    using namespace ultraterimal;
    using namespace server;
    DesktopPipelineDiagnostics diagnostics;
    const auto now = DesktopPipelineDiagnostics::Clock::now();
    const auto summary = [&](DesktopStage stage) { return diagnostics.stages[static_cast<std::size_t>(stage)].summary(); };
    wire::DesktopStageTiming timing;
    diagnostics.observe(timing);
    require(summary(DesktopStage::kWorkerQueue).samples == 0);
    timing.set_worker_queue_microseconds(0);
    timing.set_video_encode_microseconds(100);
    timing.set_source_age_microseconds(60000001);
    diagnostics.observe(timing);
    require(summary(DesktopStage::kWorkerQueue).samples == 1 && summary(DesktopStage::kWorkerQueue).meanMicroseconds == 0);
    require(summary(DesktopStage::kVideoEncode).maximumMicroseconds == 100);
    require(summary(DesktopStage::kSourceAge).samples == 0);
    wire::DesktopBrowserTiming browser;
    browser.set_canvas_submit_microseconds(0);
    diagnostics.observe(browser);
    require(summary(DesktopStage::kBrowserCanvasSubmit).samples == 1 && summary(DesktopStage::kBrowserDecode).samples == 0);
    wire::DesktopPipelineReport report;
    auto* sample = report.add_samples();
    sample->set_generation(1);
    sample->set_frame_id(5);
    sample->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
    sample->set_ipc_write_microseconds(0);
    diagnostics.report(report, now);
    require(diagnostics.pendingSamples() == 1 && summary(DesktopStage::kWorkerIpcWrite).samples == 0);
    diagnostics.generation(1, now);
    diagnostics.frame({1, 5, 0}, now + std::chrono::milliseconds(100));
    require(diagnostics.pendingSamples() == 0 && summary(DesktopStage::kWorkerIpcWrite).samples == 1 && summary(DesktopStage::kWorkerIpcWrite).meanMicroseconds == 0);
    diagnostics.report(report, now + std::chrono::milliseconds(499));
    require(summary(DesktopStage::kWorkerIpcWrite).samples == 1 && diagnostics.dropped == 1);
    diagnostics.report(report, now + std::chrono::milliseconds(500));
    require(summary(DesktopStage::kWorkerIpcWrite).samples == 2);
    sample->set_batch_id(7);
    sample->set_ipc_write_microseconds(12);
    diagnostics.report(report, now + std::chrono::seconds(1));
    require(diagnostics.pendingSamples() == 1);
    diagnostics.frame({1, 5, 7}, now + std::chrono::milliseconds(1100));
    require(summary(DesktopStage::kWorkerIpcWrite).samples == 3 && summary(DesktopStage::kWorkerIpcWrite).meanMicroseconds == 4);
    sample->set_frame_id(6);
    diagnostics.report(report, now + std::chrono::milliseconds(1500));
    diagnostics.expire(now + std::chrono::milliseconds(3499));
    require(diagnostics.pendingSamples() == 1);
    diagnostics.frame({1, 6, 7}, now + std::chrono::milliseconds(3500));
    require(diagnostics.pendingSamples() == 0 && summary(DesktopStage::kWorkerIpcWrite).samples == 3);
    sample->set_source(wire::DesktopPipelineSample::AGENT_RELAY); // Field/source mismatch.
    diagnostics.report(report, now + std::chrono::seconds(4));
    require(diagnostics.pendingSamples() == 0);
    sample->clear_ipc_write_microseconds();
    sample->set_native_write_microseconds(20);
    diagnostics.report(report, now + std::chrono::milliseconds(4500));
    require(diagnostics.pendingSamples() == 0 && summary(DesktopStage::kRelayNativeWrite).samples == 1);
    sample->set_generation(2);
    diagnostics.report(report, now + std::chrono::seconds(5));
    diagnostics.generation(3, now + std::chrono::milliseconds(5100));
    require(diagnostics.pendingSamples() == 0); // Old generation cannot be observed later.
    diagnostics.report(report, now + std::chrono::milliseconds(5500));
    require(diagnostics.pendingSamples() == 0);

    DesktopPipelineDiagnostics bounded;
    report.Clear();
    for (std::uint64_t frame = 1; frame <= 32; ++frame) {
        auto* item = report.add_samples();
        item->set_generation(1);
        item->set_frame_id(frame);
        item->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
        item->set_ipc_write_microseconds(1);
    }
    bounded.report(report, now);
    require(bounded.pendingSamples() == 16 && bounded.dropped == 16);
    bounded.expire(now + std::chrono::seconds(2));
    require(bounded.pendingSamples() == 0 && bounded.dropped == 32);
    for (auto& item : *report.mutable_samples()) {
        item.set_frame_id(1);
    }
    bounded.report(report, now + std::chrono::seconds(3));
    require(bounded.pendingSamples() == 32);
    bounded.report(report, now + std::chrono::milliseconds(3500));
    require(bounded.pendingSamples() == 32 && bounded.dropped == 64);
    bounded.expire(now + std::chrono::seconds(5));
    // Byte budget includes wire unknown fields without retaining their payload.
    report.mutable_samples(0)->GetReflection()->MutableUnknownFields(report.mutable_samples(0))->AddLengthDelimited(100, std::string(8193, 'x'));
    bounded.report(report, now + std::chrono::seconds(6));
    require(bounded.pendingSamples() == 31 && bounded.dropped == 97);
    report.add_samples();
    bounded.report(report, now + std::chrono::seconds(7));
    require(bounded.pendingSamples() == 31 && bounded.dropped == 130);
    require(kDesktopStageNames[static_cast<std::size_t>(DesktopStage::kServerVideoWsWrite)] == "server_video_ws_write");
}
} // namespace
int main() {
    try {
        pipeline();
        stagePresence();
        {
            using namespace std::chrono_literals;
            ultraterimal::server::DesktopCadenceWindow cadence;
            const auto now = std::chrono::steady_clock::time_point(1s);
            ultraterimal::wire::DesktopStageTiming stages;
            stages.set_gpu_sync_wait_microseconds(34000);
            cadence.frame(stages, now);
            cadence.frame(stages, now + 50ms);
            ultraterimal::wire::DesktopFlowSample flow;
            flow.set_input_events(7);
            flow.set_input_apply_max_microseconds(12000);
            flow.set_blocking_flags(4);
            flow.set_credit_wait_microseconds(200000);
            require(cadence.observeFlow(flow, 1, 1));
            require(!cadence.observeFlow(flow, 1, 1));
            require(cadence.appliedInputs == 7 && cadence.credit.summary().samples == 1 && cadence.blockingFlags == 4);
            require(!cadence.due(now + 999ms) && cadence.due(now + 1s));
            require(cadence.fpsMilli(cadence.received, now + 1s) == 2000 && cadence.gaps.summary().maximumMicroseconds == 50000);
            require(cadence.map.summary().samples == 2 && cadence.inputApply.summary().maximumMicroseconds == 12000 && cadence.loopLate.summary().samples == 0);
            cadence.clear(now + 1s);
            require(!cadence.hasFlow && cadence.received == 0 && cadence.blockingFlags == 0 && cadence.appliedInputs == 0);
            require(!cadence.observeFlow(flow, 1, 1));
            flow.set_owner_loop_late_microseconds(0);
            require(cadence.observeFlow(flow, 1, 2) && cadence.loopLate.summary().samples == 1);
        }
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
