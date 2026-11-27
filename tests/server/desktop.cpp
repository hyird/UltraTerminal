#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <event_loop.h>
#include <exception>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <task_scope.h>
#include <utility>
#include <vector>

#include "features/desktop/desktop.runtime.h"
#include "features/desktop/desktop_exchange.runtime.h"

namespace {
using namespace ultraterimal;
constexpr std::size_t kSimulatedFrameBytes = 256 * 1024;
void require(bool condition, const std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error("Desktop pairing invariant failed at line " + std::to_string(location.line()));
    }
}
server::ChannelAuthorization authorization(bool video = false) {
    return {"session", video ? "video" : "input", "user", "device", wire::DESKTOP,
            video ? wire::DESKTOP_VIDEO : wire::DESKTOP_INPUT, true};
}
wire::Envelope refresh(std::uint64_t sequence) {
    wire::Envelope message;
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("session");
    message.set_channel_id("input");
    message.set_sequence(sequence);
    message.mutable_request_refresh();
    return message;
}
ruvia::task<void> awaitClose(server::DesktopEndpoint& endpoint, bool& finished) {
    require(!(co_await endpoint.receive({})));
    finished = true;
}
ruvia::task<void> forwardBurst(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, ruvia::worker_ref caller, bool& finished) {
    for (std::uint64_t sequence = 10; sequence <= 100; ++sequence) {
        require(caller.is_current());
        require(co_await session.forward(sender, refresh(sequence)));
        require(caller.is_current());
    }
    finished = true;
}
ruvia::task<void> forwardVideoBurst(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, wire::Envelope frame, bool& finished) {
    for (std::uint64_t index = 1; index <= 30; ++index) {
        frame.set_sequence(index + 2);
        frame.set_frame_id(index);
        auto* video = frame.mutable_video_frame();
        video->set_key_frame(index == 1);
        video->set_data(std::string(kSimulatedFrameBytes, 'v'));
        video->set_timestamp_microseconds(index * 33333);
        require(co_await session.forward(sender, frame));
    }
    finished = true;
}
ruvia::task<void> awaitRejected(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, ruvia::stop_token stop, bool& finished) {
    require(!(co_await session.forward(sender, refresh(10), stop)));
    finished = true;
}
ruvia::task<std::array<std::shared_ptr<server::DesktopEndpoint>, 4>> pair(server::DesktopSession& session, bool writable = true) {
    std::array<std::shared_ptr<server::DesktopEndpoint>, 4> endpoints;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto auth = authorization(i >= 2);
        auth.writeControl = writable;
        endpoints[i] = session.attach(auth, i % 2 ? protocol::ChannelSource::kAgent : protocol::ChannelSource::kBrowser);
        require(bool(endpoints[i]));
        require(session.ready() == (i == 3));
    }
    for (auto& endpoint : endpoints) {
        auto ready = co_await endpoint->receive({});
        require(ready && ready->ready());
    }
    co_return endpoints;
}
wire::Envelope configuration(std::uint64_t sequence = 2, std::uint64_t generation = 1, std::uint32_t contract = 2) {
    auto message = refresh(sequence);
    message.set_channel_id("video");
    message.set_generation(generation);
    auto* config = message.mutable_video_config();
    config->set_width(256);
    config->set_height(64);
    config->set_codec("avc1.42C01E");
    config->set_desktop_optimization_contract(contract);
    return message;
}
wire::Envelope video(std::uint64_t sequence, std::uint64_t frame, std::uint64_t generation = 1) {
    auto message = configuration(sequence, generation);
    message.set_frame_id(frame);
    message.mutable_video_frame()->set_key_frame(frame == 1);
    message.mutable_video_frame()->set_data("frame");
    message.mutable_video_frame()->set_timestamp_microseconds(frame);
    return message;
}
wire::Envelope pipelineReport(std::uint64_t sequence = 2, std::uint64_t frame = 1) {
    auto message = refresh(sequence);
    auto* sample = message.mutable_desktop_pipeline_report()->add_samples();
    sample->set_generation(1);
    sample->set_frame_id(frame);
    sample->set_source(wire::DesktopPipelineSample::AGENT_RELAY);
    sample->set_native_write_microseconds(0);
    return message;
}
ruvia::task<void> requestOptimization(server::DesktopSession& session, const std::array<std::shared_ptr<server::DesktopEndpoint>, 4>& endpoints, std::uint32_t contract = 2) {
    auto capabilities = refresh(2);
    capabilities.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(contract);
    require(co_await session.forward(endpoints[0], capabilities));
    require(bool(co_await endpoints[1]->receive({})));
}
wire::DesktopStageTiming workerStages() {
    wire::DesktopStageTiming timing;
    timing.set_worker_queue_microseconds(0);
    timing.set_acquire_wait_microseconds(1);
    timing.set_source_age_microseconds(2);
    timing.set_pacing_credit_wait_microseconds(3);
    timing.set_gpu_sync_wait_microseconds(4);
    timing.set_readback_ready_microseconds(5);
    timing.set_prepare_cpu_microseconds(6);
    timing.set_video_encode_microseconds(0);
    return timing;
}
struct TestDesktopWriter {
    ruvia::worker_ref worker;
    std::vector<wire::Envelope> messages;
    bool aborted = false;
    ruvia::task<void> binary(std::string_view bytes) {
        require(worker.is_current());
        auto message = protocol::decodeMessage(bytes);
        require(bool(message));
        messages.push_back(std::move(*message));
        require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(2)) == ruvia::sleep_status::elapsed);
        require(worker.is_current());
    }
    void abort() { aborted = true; }
};
ruvia::task<void> exerciseWriters(ruvia::worker_ref worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    auto owner = std::make_shared<server::DesktopSession>(worker, budget);
    auto& session = *owner;
    std::array<server::ChannelState, 4> states;
    std::array<TestDesktopWriter, 4> writers{{{worker, {}, false}, {worker, {}, false}, {worker, {}, false}, {worker, {}, false}}};
    {
        const auto scopeWorker = worker;
        ruvia::task_scope scope(scopeWorker);
        bool scopeJoined = false;
        std::exception_ptr scopeFailure;
        try {
            for (std::size_t slot = 0; slot < states.size(); ++slot) {
                auto& state = states[slot];
                state.authorization = authorization(slot >= 2);
                state.source = slot % 2 ? protocol::ChannelSource::kAgent : protocol::ChannelSource::kBrowser;
                state.session = owner;
                state.endpoint = session.attach(*state.authorization, state.source);
                require(bool(state.endpoint));
            }
            for (std::size_t slot = 0; slot < states.size(); ++slot) {
                scope.spawn(server::writeChannel(writers[slot], states[slot], scope));
            }
            require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10)) == ruvia::sleep_status::elapsed);
            for (std::size_t slot = 0; slot < writers.size(); ++slot) {
                require(writers[slot].messages.size() == 1 && writers[slot].messages[0].has_channel_ready());
                require(writers[slot].messages[0].channel_ready().desktop_optimization_contract() == 2);
                require(writers[slot].messages[0].channel_ready().purpose() == (slot >= 2 ? wire::DESKTOP_VIDEO : wire::DESKTOP_INPUT));
            }
            auto caps = refresh(2);
            caps.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(2);
            require(co_await session.forward(states[0].endpoint, caps));
            require(co_await session.forward(states[3].endpoint, configuration()));
            require(co_await session.forward(states[3].endpoint, video(3, 1)));
            require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20)) == ruvia::sleep_status::elapsed);
            const auto& pipeline = session.diagnostics().pipeline;
            const auto writes = pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary();
            require(writes.samples == 2 && writes.meanMicroseconds >= 1000); // Completion, not dequeue, measured by real writer code.
            require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoQueueWait)].summary().samples == 2);
            require(writers[2].messages.size() == 3 && writers[0].messages.size() == 1);
            auto frame = video(4, 2);
            *frame.mutable_video_frame()->mutable_stage_timing() = workerStages();
            require(co_await session.forward(states[3].endpoint, frame));
            require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20)) == ruvia::sleep_status::elapsed);
            for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
                require(pipeline.stages[index].summary().samples == 1);
            }
            require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kWorkerQueue)].summary().meanMicroseconds == 0);
            require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == writes.samples + 1);
            require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoQueueWait)].summary().samples == 3);
            require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoAdmission)].summary().samples == 3);
            require(writers[2].messages.size() == 4);
            require(writers[2].messages[3].SerializeAsString() == frame.SerializeAsString());
            auto report = pipelineReport(2, 2);
            report.mutable_desktop_pipeline_report()->clear_samples();
            auto* ipc = report.mutable_desktop_pipeline_report()->add_samples();
            ipc->set_generation(1);
            ipc->set_frame_id(2);
            ipc->set_batch_id(0);
            ipc->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
            ipc->set_ipc_write_microseconds(0);
            auto* native = report.mutable_desktop_pipeline_report()->add_samples();
            native->set_generation(1);
            native->set_frame_id(2);
            native->set_batch_id(0);
            native->set_source(wire::DesktopPipelineSample::AGENT_RELAY);
            native->set_send_queue_wait_microseconds(0);
            native->set_native_write_microseconds(0);
            require(co_await session.forward(states[1].endpoint, report));
            for (const auto stage : {server::DesktopStage::kWorkerIpcWrite, server::DesktopStage::kRelaySendQueueWait, server::DesktopStage::kRelayNativeWrite}) {
                const auto summary = pipeline.stages[static_cast<std::size_t>(stage)].summary();
                require(summary.samples == 1 && summary.meanMicroseconds == 0);
            }
            session.close();
            scopeJoined = true;
            co_await scope.join();
            for (const auto& writer : writers) {
                require(writer.aborted);
            }
            require(budget->used == 0);

        } catch (...) {
            scopeFailure = std::current_exception();
        }
        if (!scopeJoined) {
            scope.request_stop();
            try {
                co_await scope.join();
            } catch (...) {
                if (!scopeFailure) {
                    scopeFailure = std::current_exception();
                }
            }
        }
        if (scopeFailure) {
            std::rethrow_exception(scopeFailure);
        }
    }
}
ruvia::task<void> exerciseOptimization(ruvia::worker_ref worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session, false);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[1], pipelineReport())); // Report can overtake even config.
        require(session.diagnostics().pipeline.pendingSamples() == 1 && budget->used == 0);
        require(co_await session.forward(endpoints[3], configuration()));
        auto delivered = co_await endpoints[2]->receive({});
        require(delivered && session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == 0);
        session.writeCompleted(*delivered, std::chrono::microseconds(1000));
        delivered.reset();
        const auto ws = session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary();
        require(ws.samples == 1 && ws.meanMicroseconds == 1000);
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoQueueWait)].summary().samples == 1);
        require(session.diagnostics().pipeline.pendingSamples() == 1 && budget->used == 0);
        auto frame = video(3, 1);
        frame.mutable_video_frame()->mutable_stage_timing()->set_video_encode_microseconds(0);
        require(co_await session.forward(endpoints[3], frame));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().pipeline.pendingSamples() == 0);
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kRelayNativeWrite)].summary().samples == 1);
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kVideoEncode)].summary().samples == 1);
        require(co_await session.forward(endpoints[1], pipelineReport(3))); // Rate-limited, not session-fatal.
        require(session.ready() && budget->used == 0 && session.diagnostics().pipeline.dropped == 1);
        require(co_await session.forward(endpoints[3], video(4, 2)));
        require(bool(co_await endpoints[2]->receive({})));
        auto feedback = refresh(3);
        auto* quality = feedback.mutable_quality_feedback();
        quality->set_displayed_generation(1);
        quality->set_displayed_frame(2);
        quality->set_decode_microseconds(123);
        quality->set_queue_depth(3);
        quality->mutable_browser_timing()->set_decode_microseconds(0);
        quality->mutable_browser_timing()->set_canvas_submit_microseconds(30);
        require(co_await session.forward(endpoints[0], feedback));
        delivered = co_await endpoints[1]->receive({});
        auto decoded = protocol::decodeMessage(delivered->bytes());
        require(decoded && !decoded->quality_feedback().has_browser_timing() && decoded->quality_feedback().decode_microseconds() == 123 && decoded->quality_feedback().queue_depth() == 3);
        delivered.reset();
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kBrowserDecode)].summary().samples == 1);
        feedback.set_sequence(4);
        quality->set_displayed_frame(3); // Unknown future canvas, stripped but unobserved.
        require(co_await session.forward(endpoints[0], feedback));
        require(bool(co_await endpoints[1]->receive({})));
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kBrowserDecode)].summary().samples == 1);
        require(co_await session.forward(endpoints[3], configuration(5, 2)));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.ready() && budget->used == 0);
        session.logHealth();
    }
    // Unknown higher versions and either missing declaration do not activate v2.
    for (const auto contracts : std::array<std::array<std::uint32_t, 2>, 5>{{{0, 2}, {1, 2}, {3, 2}, {2, 0}, {2, 3}}}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints, contracts[0]);
        require(co_await session.forward(endpoints[1], pipelineReport()));
        require(co_await session.forward(endpoints[3], configuration(2, 1, contracts[1])));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kRelayNativeWrite)].summary().samples == 0);
        for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
            require(session.diagnostics().pipeline.stages[index].summary().samples == 0);
        }
        auto timed = video(4, 2);
        timed.mutable_video_frame()->mutable_stage_timing()->set_worker_queue_microseconds(0);
        require(!(co_await session.forward(endpoints[3], timed)));
    }
    for (int diagnostic = 0; diagnostic < 3; ++diagnostic) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        if (diagnostic == 0) {
            require(co_await session.forward(endpoints[1], pipelineReport()));
            require(session.ready() && budget->used == 0 && session.diagnostics().pipeline.dropped == 1);
        } else if (diagnostic == 1) {
            auto feedback = refresh(2);
            feedback.mutable_quality_feedback()->set_displayed_generation(1);
            feedback.mutable_quality_feedback()->set_displayed_frame(1);
            feedback.mutable_quality_feedback()->mutable_browser_timing()->set_decode_microseconds(0);
            require(co_await session.forward(endpoints[0], feedback));
            auto delivered = co_await endpoints[1]->receive({});
            auto decoded = protocol::decodeMessage(delivered->bytes());
            require(decoded && !decoded->quality_feedback().has_browser_timing());
            require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kBrowserDecode)].summary().samples == 0);
        } else {
            auto frame = video(4, 2);
            frame.mutable_video_frame()->mutable_stage_timing()->set_worker_queue_microseconds(0);
            require(!(co_await session.forward(endpoints[3], frame)));
        }
    }
    // A request after config is deferred until the next generation barrier.
    for (bool nextGeneration : {false, true}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        co_await requestOptimization(session, endpoints);
        if (nextGeneration) {
            require(co_await session.forward(endpoints[3], configuration(3, 2)));
            require(bool(co_await endpoints[2]->receive({})));
        }
        auto frame = video(4, 1, nextGeneration ? 2 : 1);
        frame.mutable_video_frame()->mutable_stage_timing()->set_video_encode_microseconds(0);
        require((co_await session.forward(endpoints[3], frame)) == nextGeneration);
        if (nextGeneration) {
            require(bool(co_await endpoints[2]->receive({})));
        }
    }
    // Bad correlation and wrong ingress directions are rejected.
    for (int invalid = 0; invalid < 4; ++invalid) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        wire::Envelope message;
        auto sender = endpoints[0];
        switch (invalid) {
        case 0:
            message = pipelineReport(3);
            break; // Browser may never report Agent stages.
        case 1:
            message = pipelineReport(4);
            message.set_channel_id("video");
            sender = endpoints[3];
            break;
        case 2:
            message = pipelineReport(2);
            sender = endpoints[1];
            for (int index = 0; index < 32; ++index) {
                *message.mutable_desktop_pipeline_report()->add_samples() = message.desktop_pipeline_report().samples(0);
            }
            break;
        case 3:
            message = refresh(3);
            message.mutable_quality_feedback()->set_displayed_generation(1);
            message.mutable_quality_feedback()->set_displayed_frame(1);
            message.mutable_quality_feedback()->mutable_browser_timing()->set_decode_microseconds(60000001);
            break;
        }
        require(!(co_await session.forward(sender, message)) && session.closed());
        require(session.diagnostics().feedbackMessages == 0);
    }
    require(budget->used == 0);
}
ruvia::task<void> exercise(ruvia::worker_ref worker) {
    {
        auto continuousBudget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
        server::DesktopSession session(worker, continuousBudget);
        auto endpoints = co_await pair(session);
        auto caps = refresh(2);
        caps.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(2);
        caps.mutable_desktop_video_capabilities()->set_continuous_frames(true);
        require(co_await session.forward(endpoints[0], caps));
        require(bool(co_await endpoints[1]->receive({})));
        auto config = configuration();
        config.mutable_video_config()->set_continuous_frames(true);
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        auto repeat = video(4, 2);
        repeat.set_base_frame_id(1);
        repeat.mutable_video_frame()->set_repeated(true);
        require(co_await session.forward(endpoints[3], repeat));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().videoFrames == 2);
        repeat.set_sequence(5);
        repeat.set_frame_id(3);
        repeat.set_base_frame_id(2);
        repeat.mutable_video_frame()->set_timestamp_microseconds(3);
        require(!(co_await session.forward(endpoints[3], repeat))); // A delivery alias cannot replace the content key.
    }
    co_await exerciseWriters(worker);
    co_await exerciseOptimization(worker);
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    {
        server::DesktopSession session(worker, budget);
        auto browser = session.attach(authorization(), protocol::ChannelSource::kBrowser);
        require(bool(browser));
        require(!(co_await session.forward(browser, refresh(2))));
        require(!session.attach(authorization(), protocol::ChannelSource::kBrowser));
        for (int mismatch = 0; mismatch < 6; ++mismatch) {
            auto auth = authorization();
            switch (mismatch) {
            case 0:
                auth.sessionId = "other";
                break;
            case 1:
                auth.userId = "other";
                break;
            case 2:
                auth.deviceId = "other";
                break;
            case 3:
                auth.channelId = "other";
                break;
            case 4:
                auth.writeControl = false;
                break;
            case 5:
                auth.kind = wire::SSH;
                break;
            }
            require(!session.attach(auth, protocol::ChannelSource::kAgent));
        }
        ruvia::stop_source stop;
        stop.request_stop();
        require(!(co_await browser->receive(stop.token())));
        session.close();
        require(!(co_await browser->receive({})));
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto feedback = refresh(2);
        feedback.mutable_quality_feedback()->set_decode_microseconds(18000);
        auto config = refresh(2);
        config.set_channel_id("video");
        config.set_generation(1);
        config.mutable_video_config()->set_width(16);
        config.mutable_video_config()->set_height(16);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        auto frame = config;
        frame.set_sequence(3);
        frame.set_frame_id(1);
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data("frame");
        require(co_await session.forward(endpoints[3], frame));
        require(bool(co_await endpoints[2]->receive({})));
        feedback.mutable_quality_feedback()->set_displayed_generation(1);
        feedback.mutable_quality_feedback()->set_displayed_frame(1);
        feedback.mutable_quality_feedback()->set_queue_depth(4);
        feedback.mutable_quality_feedback()->set_background(true);
        require(co_await session.forward(endpoints[0], feedback));
        require(session.diagnostics().feedbackMessages == 1);
        require(session.diagnostics().maximumDecodeMicroseconds == 18000);
        require(session.diagnostics().latestBrowserQueueDepth == 4);
        require(session.diagnostics().latestBackground);
        feedback.set_sequence(3);
        feedback.mutable_quality_feedback()->set_background(false);
        auto* timing = feedback.mutable_quality_feedback()->mutable_visible_frame_timing();
        timing->set_sample_count(120);
        timing->set_p95_microseconds(48000);
        timing->set_p99_microseconds(63000);
        timing->set_long_gap_count(2);
        timing->set_observed_fps_milli(27000);
        require(co_await session.forward(endpoints[0], feedback));
        require(session.diagnostics().visibleFrames.reports == 1 && session.diagnostics().visibleFrames.latestSamples == 120 &&
                session.diagnostics().visibleFrames.latestP95Microseconds == 48000 && session.diagnostics().visibleFrames.latestLongGaps == 2 &&
                session.diagnostics().visibleFrames.latestFpsMilli == 27000);
        session.logHealth();
        require(session.diagnostics().visibleFrames.reports == 0);
        session.close(server::DesktopCloseReason::kPeerClosed, server::DesktopCloseOrigin::kBrowserInput);
        session.close(server::DesktopCloseReason::kReadFailed, server::DesktopCloseOrigin::kAgentVideo);
        require(session.closeReason() == server::DesktopCloseReason::kPeerClosed);
        require(session.closeOrigin() == server::DesktopCloseOrigin::kBrowserInput);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        auto inFlight = co_await endpoints[1]->receive({});
        require(inFlight && !inFlight->ready());
        const auto encoded = protocol::encodeMessage(refresh(2));
        require(encoded && inFlight->bytes() == *encoded);
        const std::size_t occupied = budget->used.load();
        require(occupied > inFlight->bytes().size());
        require(co_await session.forward(endpoints[0], refresh(10)));
        require(budget->used > occupied); // Receiving is not completion of a socket write.
        inFlight.reset();
        require(budget->used == occupied);
        for (std::uint64_t sequence = 3; sequence <= 10; ++sequence) {
            auto delivery = co_await endpoints[1]->receive({});
            const auto message = delivery ? protocol::decodeMessage(delivery->bytes()) : std::unexpected(protocol::MessageError::kInvalidField);
            require(message && message->sequence() == sequence);
        }
        require(budget->used == 0);
        require(!(co_await session.forward(endpoints[0], refresh(10)))); // Replay closes every endpoint.
        require(!session.ready());
        require(session.closeReason() == server::DesktopCloseReason::kMessageReplay);
        require(session.closeOrigin() == server::DesktopCloseOrigin::kBrowserInput);
        require(session.diagnostics().forwardedMessages == 9);
        require(session.diagnostics().peakPendingBytes > 0);
        for (auto& endpoint : endpoints) {
            require(!(co_await endpoint->receive({})));
        }
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        bool finished = false;
        {
            const auto scopeWorker = worker;
            ruvia::task_scope scope(scopeWorker);
            bool scopeJoined = false;
            std::exception_ptr scopeFailure;
            try {
                scope.spawn(forwardBurst(session, endpoints[0], worker, finished));
                require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10)) == ruvia::sleep_status::elapsed);
                require(session.ready() && !finished);
                require(budget->used == 9 * protocol::encodeMessage(refresh(2))->size());
                for (std::uint64_t sequence = 2; sequence <= 100; ++sequence) {
                    auto delivery = co_await endpoints[1]->receive({});
                    require(delivery && delivery->bytes() == *protocol::encodeMessage(refresh(sequence)));
                }
                scopeJoined = true;
                co_await scope.join();
                require(finished && session.ready() && budget->used == 0);
                require(session.diagnostics().backpressureEvents > 0);
                require(session.diagnostics().forwardedMessages == 99);

            } catch (...) {
                scopeFailure = std::current_exception();
            }
            if (!scopeJoined) {
                scope.request_stop();
                try {
                    co_await scope.join();
                } catch (...) {
                    if (!scopeFailure) {
                        scopeFailure = std::current_exception();
                    }
                }
            }
            if (scopeFailure) {
                std::rethrow_exception(scopeFailure);
            }
        }
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto configuration = refresh(2);
        configuration.set_channel_id("video");
        configuration.set_generation(1);
        configuration.mutable_video_config()->set_width(16);
        configuration.mutable_video_config()->set_height(16);
        configuration.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], configuration));
        auto deliveredConfiguration = co_await endpoints[2]->receive({});
        require(deliveredConfiguration && deliveredConfiguration->bytes() == *protocol::encodeMessage(configuration));
        deliveredConfiguration.reset();
        bool finished = false;
        {
            const auto scopeWorker = worker;
            ruvia::task_scope scope(scopeWorker);
            bool scopeJoined = false;
            std::exception_ptr scopeFailure;
            try {
                scope.spawn(forwardVideoBurst(session, endpoints[3], configuration, finished));
                require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10)) == ruvia::sleep_status::elapsed);
                require(session.ready() && !finished && budget->used > 2 * kSimulatedFrameBytes && budget->used < 4 * kSimulatedFrameBytes);
                // A backed-up video route must not hold the separate input route.
                const auto control = refresh(2);
                require(co_await session.forward(endpoints[0], control));
                auto controlDelivery = co_await endpoints[1]->receive({});
                require(controlDelivery && controlDelivery->bytes() == *protocol::encodeMessage(control) && !finished);
                controlDelivery.reset();
                const std::string expectedFrame(kSimulatedFrameBytes, 'v');
                for (std::uint64_t index = 1; index <= 30; ++index) {
                    auto delivery = co_await endpoints[2]->receive({});
                    const auto frame = delivery ? protocol::decodeMessage(delivery->bytes()) : std::unexpected(protocol::MessageError::kInvalidField);
                    require(frame && frame->frame_id() == index && frame->sequence() == index + 2 && frame->video_frame().timestamp_microseconds() == index * 33333 &&
                            frame->video_frame().key_frame() == (index == 1) && frame->video_frame().data() == expectedFrame);
                    require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10)) == ruvia::sleep_status::elapsed);
                }
                scopeJoined = true;
                co_await scope.join();
                require(finished && session.ready() && session.diagnostics().videoFrames == 30 && session.diagnostics().backpressureEvents > 0 && budget->used == 0);
                const auto& stages = session.diagnostics().pipeline.stages;
                require(stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoAdmission)].summary().maximumMicroseconds >= 8000);
                require(stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == 0); // Receiving is not a completed WS write.

            } catch (...) {
                scopeFailure = std::current_exception();
            }
            if (!scopeJoined) {
                scope.request_stop();
                try {
                    co_await scope.join();
                } catch (...) {
                    if (!scopeFailure) {
                        scopeFailure = std::current_exception();
                    }
                }
            }
            if (scopeFailure) {
                std::rethrow_exception(scopeFailure);
            }
        }
    }
    for (bool cancel : {false, true}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        bool finished = false;
        {
            const auto scopeWorker = worker;
            ruvia::task_scope scope(scopeWorker);
            bool scopeJoined = false;
            std::exception_ptr scopeFailure;
            try {
                ruvia::stop_source stop;
                scope.spawn(awaitRejected(session, endpoints[0], stop.token(), finished));
                require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10)) == ruvia::sleep_status::elapsed);
                require(!finished && session.ready());
                if (cancel) {
                    stop.request_stop();
                } else {
                    session.close();
                }
                scopeJoined = true;
                co_await scope.join();
                require(finished && !session.ready());
                require(!(co_await endpoints[1]->receive({})));
                require(budget->used == 0);

            } catch (...) {
                scopeFailure = std::current_exception();
            }
            if (!scopeJoined) {
                scope.request_stop();
                try {
                    co_await scope.join();
                } catch (...) {
                    if (!scopeFailure) {
                        scopeFailure = std::current_exception();
                    }
                }
            }
            if (scopeFailure) {
                std::rethrow_exception(scopeFailure);
            }
        }
    }
    require(budget->used == 0);
    {
        auto session = std::make_unique<server::DesktopSession>(worker, budget);
        auto endpoints = co_await pair(*session);
        std::array<bool, 4> finished{};
        {
            const auto scopeWorker = worker;
            ruvia::task_scope scope(scopeWorker);
            bool scopeJoined = false;
            std::exception_ptr scopeFailure;
            try {
                for (std::size_t i = 0; i < endpoints.size(); ++i) {
                    scope.spawn(awaitClose(*endpoints[i], finished[i]));
                }
                require(co_await ruvia::sleep_for(worker, std::chrono::milliseconds(1)) == ruvia::sleep_status::elapsed);
                session.reset(); // Destruction must wake all four writers while endpoint handles survive.
                scopeJoined = true;
                co_await scope.join();
                for (bool complete : finished) {
                    require(complete);
                }

            } catch (...) {
                scopeFailure = std::current_exception();
            }
            if (!scopeJoined) {
                scope.request_stop();
                try {
                    co_await scope.join();
                } catch (...) {
                    if (!scopeFailure) {
                        scopeFailure = std::current_exception();
                    }
                }
            }
            if (scopeFailure) {
                std::rethrow_exception(scopeFailure);
            }
        }
    }
    {
        auto small = std::make_shared<server::DesktopBudget>(protocol::encodeMessage(refresh(2))->size());
        server::DesktopSession first(worker, small);
        server::DesktopSession second(worker, small);
        auto one = co_await pair(first);
        auto two = co_await pair(second);
        require(co_await first.forward(one[0], refresh(2)));
        auto inFlight = co_await one[1]->receive({});
        require(inFlight && small->used == small->limit);
        require(!(co_await second.forward(two[0], refresh(2)))); // Shared limit includes another session's write.
        require(!second.ready() && first.ready());
        require(second.closeReason() == server::DesktopCloseReason::kGlobalBudgetExceeded);
        first.close();
        require(small->used == small->limit);
        inFlight.reset();
        require(small->used == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto config = refresh(2);
        config.set_channel_id("video");
        config.set_generation(1);
        config.mutable_video_config()->set_width(16);
        config.mutable_video_config()->set_height(16);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        auto frame = config;
        frame.set_sequence(3);
        frame.set_frame_id(1);
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data("frame");
        frame.mutable_video_frame()->set_timestamp_microseconds(1);
        require(co_await session.forward(endpoints[3], frame));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().encoding.summary().samples == 0); // Legacy Agent.
        require(session.diagnostics().latestTargetFramesPerSecond == 0 && session.diagnostics().latestTargetBitrate == 0);
        frame.set_sequence(4);
        frame.set_frame_id(2);
        frame.mutable_video_frame()->set_timestamp_microseconds(2);
        frame.mutable_video_frame()->set_encode_microseconds(1234);
        frame.mutable_video_frame()->set_hardware_encoding(true);
        frame.mutable_video_frame()->set_target_frames_per_second(30);
        frame.mutable_video_frame()->set_target_bitrate(4000000);
        require(co_await session.forward(endpoints[3], frame));
        auto received = co_await endpoints[2]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(frame));
        received.reset();
        require(session.diagnostics().encoding.summary().samples == 1 && session.diagnostics().latestHardwareEncoding);
        require(session.diagnostics().latestTargetFramesPerSecond == 30 && session.diagnostics().latestTargetBitrate == 4000000);
        auto feedback = refresh(2);
        auto* quality = feedback.mutable_quality_feedback();
        quality->set_displayed_generation(1);
        quality->set_displayed_frame(2);
        auto* report = quality->mutable_visible_frame_timing();
        report->set_sample_count(48);
        report->set_p95_microseconds(258000);
        report->set_p99_microseconds(300000);
        report->set_long_gap_count(5);
        report->set_observed_fps_milli(20000);
        require(co_await session.forward(endpoints[0], feedback));
        received = co_await endpoints[1]->receive({});
        const auto forwarded = protocol::decodeMessage(received->bytes());
        require(forwarded && !forwarded->quality_feedback().has_visible_frame_timing());
        received.reset();
        require(session.diagnostics().visibleFrames.reports == 1 && session.diagnostics().visibleFrames.latestSamples == 48 && session.diagnostics().visibleFrames.latestP95Microseconds == 258000 && session.diagnostics().visibleFrames.latestLongGaps == 5);
        feedback.set_sequence(3);
        report->set_p95_microseconds(1000);
        require(co_await session.forward(endpoints[0], feedback));
        require(bool(co_await endpoints[1]->receive({})));
        require(session.diagnostics().visibleFrames.reports == 1 && session.diagnostics().visibleFrames.latestP95Microseconds == 258000); // Rate limited.
        feedback.set_sequence(4);
        const auto reportSnapshot = *report;
        quality->clear_visible_frame_timing();
        quality->set_background(true);
        require(co_await session.forward(endpoints[0], feedback));
        require(bool(co_await endpoints[1]->receive({})));
        require(session.diagnostics().renderReportedAt == std::chrono::steady_clock::time_point{});
        feedback.set_sequence(5);
        quality->set_background(false);
        quality->set_displayed_frame(3);
        *quality->mutable_visible_frame_timing() = reportSnapshot;
        require(co_await session.forward(endpoints[0], feedback));
        require(bool(co_await endpoints[1]->receive({})));
        require(session.diagnostics().visibleFrames.reports == 0); // Unknown future frame cannot supply metrics.
        config.set_sequence(5);
        config.set_generation(2);
        config.mutable_video_config()->set_width(32);
        require(co_await session.forward(endpoints[3], config));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().encoding.summary().samples == 0 && session.diagnostics().videoWidth == 32);
        require(session.diagnostics().latestKeyFrameEncodeMicroseconds == 0);
        require(session.diagnostics().latestTargetFramesPerSecond == 0 && session.diagnostics().latestTargetBitrate == 0);
        require(session.diagnostics().renderReportedAt == std::chrono::steady_clock::time_point{});
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        auto config = refresh(2);
        config.set_channel_id("video");
        config.set_generation(1);
        config.mutable_video_config()->set_width(1920);
        config.mutable_video_config()->set_height(1080);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        require(co_await session.forward(endpoints[3], config));
        auto received = co_await endpoints[2]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(config));
        received.reset();
        auto frame = config;
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data(std::string(8 * 1024 * 1024, 'v'));
        for (std::uint64_t i = 1; i <= 3; ++i) {
            frame.set_sequence(i + 2);
            frame.set_frame_id(i);
            frame.mutable_video_frame()->set_timestamp_microseconds(i);
            frame.mutable_video_frame()->set_encode_microseconds(static_cast<std::uint32_t>(i * 1000));
            require(co_await session.forward(endpoints[3], frame));
            if (i == 1) {
                // One active write plus two queued frames must all retain their
                // budget; dequeuing is not completion of a network write.
                received = co_await endpoints[2]->receive({});
                require(bool(received));
            }
        }
        require(budget->used > 24 * 1024 * 1024);
        frame.set_sequence(6);
        frame.set_frame_id(4);
        frame.mutable_video_frame()->set_timestamp_microseconds(4);
        require(!(co_await session.forward(endpoints[3], frame))); // Byte budget includes the active write.
        require(!session.ready());
        require(session.closeReason() == server::DesktopCloseReason::kSessionBudgetExceeded);
        require(session.diagnostics().videoFrames == 3);
        const auto encoding = session.diagnostics().encoding.summary();
        require(encoding.samples == 3 && encoding.meanMicroseconds == 2000 && encoding.p95Microseconds == 3000 && encoding.maximumMicroseconds == 3000);
        require(session.diagnostics().videoWidth == 1920 && session.diagnostics().videoHeight == 1080);
        require(session.diagnostics().latestKeyFrameEncodeMicroseconds == 3000 && !session.diagnostics().latestHardwareEncoding);
        require(!(co_await endpoints[2]->receive({})));
        received.reset();
        require(budget->used == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session, false);
        require(co_await session.forward(endpoints[0], refresh(2)));
        require(bool(co_await endpoints[1]->receive({})));
        auto cursor = refresh(2);
        cursor.mutable_cursor_update()->set_visible(false);
        require(co_await session.forward(endpoints[1], cursor));
        auto received = co_await endpoints[0]->receive({});
        require(received && received->bytes() == *protocol::encodeMessage(cursor));
        received.reset();
        auto input = refresh(3);
        input.set_generation(1);
        input.mutable_input_event()->set_kind(wire::InputEvent::KEY);
        input.mutable_input_event()->set_code(65);
        require(!(co_await session.forward(endpoints[0], input)));
        require(!session.ready());
    }
}
ruvia::task<void> attachCrossWorkerEndpoints(std::shared_ptr<server::DesktopSession> session, std::array<std::shared_ptr<server::DesktopEndpoint>, 4>* endpoints, std::array<ruvia::worker_ref, 4> ioWorkers) {
    for (std::size_t i = 0; i < endpoints->size(); ++i) {
        (*endpoints)[i] = session->attach(authorization(i >= 2), i % 2 ? protocol::ChannelSource::kAgent : protocol::ChannelSource::kBrowser, ioWorkers[i]);
        require(bool((*endpoints)[i]));
    }
    require(session->ready());
    co_return;
}
ruvia::task<void> awaitReadyEndpoint(std::shared_ptr<server::DesktopEndpoint> endpoint) {
    auto ready = co_await endpoint->receive({});
    require(ready && ready->ready());
    co_return;
}
ruvia::task<void> runCrossWorkerProducer(std::shared_ptr<server::DesktopSession> session, std::shared_ptr<server::DesktopEndpoint> agentVideoEndpoint) {
    require(co_await session->forward(agentVideoEndpoint, configuration()));
    auto frame = video(3, 1);
    frame.mutable_video_frame()->set_data(std::string(64 * 1024, 'x'));
    for (std::uint64_t index = 1; index <= 20; ++index) {
        frame.set_sequence(index + 2);
        frame.set_frame_id(index);
        frame.mutable_video_frame()->set_key_frame(index == 1);
        frame.mutable_video_frame()->set_timestamp_microseconds(index * 16666);
        require(co_await session->forward(agentVideoEndpoint, frame));
    }
    co_return;
}
ruvia::task<void> runCrossWorkerConsumer(std::shared_ptr<server::DesktopEndpoint> browserVideoEndpoint) {
    auto deliveredConfig = co_await browserVideoEndpoint->receive({});
    require(bool(deliveredConfig));
    deliveredConfig.reset();
    for (std::uint64_t index = 1; index <= 20; ++index) {
        auto delivered = co_await browserVideoEndpoint->receive({});
        require(bool(delivered));
        const auto parsed = protocol::decodeMessage(delivered->bytes());
        require(parsed && parsed->frame_id() == index);
    }
    co_return;
}
ruvia::task<void> closeCrossWorkerSession(std::shared_ptr<server::DesktopSession> session) {
    session->close();
    co_return;
}
ruvia::task<void> drainClosedEndpoint(std::shared_ptr<server::DesktopEndpoint> endpoint) {
    require(!(co_await endpoint->receive({})));
    co_return;
}
void exerciseCrossWorker() {
    using namespace ultraterimal;
    ruvia::event_loop_pool cluster({.loop_count_ = 4});
    cluster.start();
    const auto loop0 = cluster.loop(0);
    const auto loop1 = cluster.loop(1);
    const auto loop2 = cluster.loop(2);
    const auto loop3 = cluster.loop(3);
    const auto sessionWorker = loop0.worker();
    const std::array<ruvia::worker_ref, 4> ioWorkers{
        loop1.worker(), loop2.worker(), loop3.worker(), loop1.worker()};
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    auto session = std::make_shared<server::DesktopSession>(sessionWorker, budget);
    std::array<std::shared_ptr<server::DesktopEndpoint>, 4> endpoints;
    auto startTask = loop0.submit(attachCrossWorkerEndpoints(session, &endpoints, ioWorkers)).take_value();
    std::move(startTask).take_value();
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto readyTask = cluster.loop(i == 3 ? 1 : i + 1).submit(awaitReadyEndpoint(endpoints[i])).take_value();
        std::move(readyTask).take_value();
    }
    // Concurrent cross-worker video burst exceeding channel queue capacity (=2) to verify backpressure wakeup across cores.
    auto producer = loop0.submit(runCrossWorkerProducer(session, endpoints[3])).take_value();
    auto consumer = loop3.submit(runCrossWorkerConsumer(endpoints[2])).take_value();
    std::move(producer).take_value();
    std::move(consumer).take_value();
    require(budget->used.load() == 0);
    auto closeTask = loop0.submit(closeCrossWorkerSession(session)).take_value();
    std::move(closeTask).take_value();
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto drainTask = cluster.loop(i == 3 ? 1 : i + 1).submit(drainClosedEndpoint(endpoints[i])).take_value();
        std::move(drainTask).take_value();
    }
    cluster.join();
}
} // namespace
int main() {
    try {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        const auto loop0 = pool.loop(0);
        auto task = loop0.submit(exercise(loop0.worker())).take_value();
        std::move(task).take_value();
        pool.join();
        exerciseCrossWorker();
        std::cout << "Desktop four-endpoint pairing, cross-worker sharding, FIFO, replay, read-only access and bounded in-flight ownership passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
