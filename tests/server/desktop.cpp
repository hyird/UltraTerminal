#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/execution_domain.h>
#include <ruvia/core/io_runtime.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

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
ruvia::task<void> forwardBurst(server::DesktopSession& session, std::shared_ptr<server::DesktopEndpoint> sender, ruvia::execution_handle caller, bool& finished) {
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
wire::Envelope refinement(std::uint64_t sequence = 4, std::uint64_t frame = 1, std::uint64_t batch = 1) {
    auto message = configuration(sequence);
    message.set_frame_id(frame);
    message.set_base_frame_id(frame);
    auto* detail = message.mutable_video_refinement();
    detail->set_batch_id(batch);
    detail->set_kind(batch ? wire::STATIC_DETAIL : wire::UNSPECIFIED);
    auto* patch = detail->add_patches();
    patch->mutable_destination()->set_width(128);
    patch->mutable_destination()->set_height(64);
    std::string png("\211PNG\r\n\032\n", 8);
    png.resize(33);
    png[11] = 13;
    png.replace(12, 4, "IHDR");
    png[19] = static_cast<char>(128);
    png[23] = 64;
    patch->set_png(png); // Protocol header fixture; no actual decoding here.
    return message;
}
wire::Envelope refinementAck(std::uint64_t sequence = 3, std::uint64_t frame = 1, std::uint64_t batch = 1) {
    auto message = refresh(sequence);
    message.set_generation(1);
    message.set_frame_id(frame);
    message.mutable_desktop_refinement_ack()->set_batch_id(batch);
    message.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::APPLIED);
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
    timing.set_png_encode_microseconds(7);
    timing.set_video_encode_microseconds(0);
    return timing;
}
struct TestDesktopWriter {
    ruvia::execution_handle worker;
    std::vector<wire::Envelope> messages;
    bool aborted = false;
    ruvia::task<void> binary(std::string_view bytes) {
        require(worker.is_current());
        auto message = protocol::decodeMessage(bytes);
        require(bool(message));
        messages.push_back(std::move(*message));
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(2));
        require(worker.is_current());
    }
    void abort() { aborted = true; }
};
ruvia::task<void> exerciseWriters(ruvia::execution_handle worker) {
    ruvia::execution_domain browserInput(worker.io_context()), agentInput(worker.io_context()), browserVideo(worker.io_context()), agentVideo(worker.io_context());
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    auto owner = std::make_shared<server::DesktopSession>(worker, budget);
    auto& session = *owner;
    std::array<server::ChannelState, 4> states;
    std::array<TestDesktopWriter, 4> writers{{{browserInput.handle(), {}, false}, {agentInput.handle(), {}, false}, {browserVideo.handle(), {}, false}, {agentVideo.handle(), {}, false}}};
    co_await ruvia::with_task_scope(worker, [&](ruvia::task_scope& scope) -> ruvia::task<void> {
        for (std::size_t slot = 0; slot < states.size(); ++slot) {
            auto& state = states[slot];
            state.authorization = authorization(slot >= 2);
            state.source = slot % 2 ? protocol::ChannelSource::kAgent : protocol::ChannelSource::kBrowser;
            state.session = owner;
            state.endpoint = session.attach(*state.authorization, state.source);
            require(bool(state.endpoint));
        }
        for (std::size_t slot = 0; slot < states.size(); ++slot) {
            scope.spawn(ruvia::on(writers[slot].worker, server::writeChannel(writers[slot], states[slot], scope)));
        }
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
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
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
        const auto& pipeline = session.diagnostics().pipeline;
        const auto writes = pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary();
        require(writes.samples == 2 && writes.meanMicroseconds >= 1000); // Completion, not dequeue, measured by real writer code.
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoQueueWait)].summary().samples == 2);
        require(writers[2].messages.size() == 3 && writers[0].messages.size() == 1);
        auto frame = video(4, 2);
        *frame.mutable_video_frame()->mutable_stage_timing() = workerStages();
        auto dynamic = refinement(5, 2);
        dynamic.mutable_video_refinement()->set_kind(wire::DYNAMIC_DETAIL);
        *dynamic.mutable_video_refinement()->mutable_stage_timing() = workerStages();
        require(co_await session.forward(states[3].endpoint, frame));
        require(co_await session.forward(states[3].endpoint, dynamic));
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
        for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
            require(pipeline.stages[index].summary().samples == 1);
        }
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kWorkerQueue)].summary().meanMicroseconds == 0);
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kPngEncode)].summary().meanMicroseconds == 7);
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == writes.samples + 2);
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoQueueWait)].summary().samples == 4);
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoAdmission)].summary().samples == 4);
        require(writers[2].messages.size() == 5);
        require(writers[2].messages[3].SerializeAsString() == frame.SerializeAsString());
        require(writers[2].messages[4].SerializeAsString() == dynamic.SerializeAsString());
        auto report = pipelineReport(2, 2);
        report.mutable_desktop_pipeline_report()->clear_samples();
        for (const auto batch : {0u, 1u}) {
            auto* ipc = report.mutable_desktop_pipeline_report()->add_samples();
            ipc->set_generation(1);
            ipc->set_frame_id(2);
            ipc->set_batch_id(batch);
            ipc->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
            ipc->set_ipc_write_microseconds(0);
            auto* native = report.mutable_desktop_pipeline_report()->add_samples();
            native->set_generation(1);
            native->set_frame_id(2);
            native->set_batch_id(batch);
            native->set_source(wire::DesktopPipelineSample::AGENT_RELAY);
            native->set_send_queue_wait_microseconds(0);
            native->set_native_write_microseconds(0);
        }
        require(co_await session.forward(states[1].endpoint, report));
        for (const auto stage : {server::DesktopStage::kWorkerIpcWrite, server::DesktopStage::kRelaySendQueueWait, server::DesktopStage::kRelayNativeWrite}) {
            const auto summary = pipeline.stages[static_cast<std::size_t>(stage)].summary();
            require(summary.samples == 2 && summary.meanMicroseconds == 0);
        }
        require(co_await session.forward(states[0].endpoint, refinementAck(3, 2)));
        auto detail = refinement(6, 2, 2);                                                        // A separate static worker invocation on the same base.
        detail.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_x(128); // Not the already-refined dynamic tile.
        *detail.mutable_video_refinement()->mutable_stage_timing() = workerStages();
        require(co_await session.forward(states[3].endpoint, detail));
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(20));
        for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
            require(pipeline.stages[index].summary().samples == 2);
        }
        require(pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == 5);
        require(writers[2].messages.size() == 6 && writers[2].messages.back().SerializeAsString() == detail.SerializeAsString());
        session.close();
        co_await scope.join();
        for (const auto& writer : writers) {
            require(writer.aborted);
        }
        require(budget->used == 0);
    });
}
ruvia::task<void> exerciseDynamicStagePresence(ruvia::execution_handle worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    for (int presence = 0; presence < 6; ++presence) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        auto initial = video(3, 1);
        *initial.mutable_video_frame()->mutable_stage_timing() = workerStages();
        require(co_await session.forward(endpoints[3], initial));
        require(bool(co_await endpoints[2]->receive({})));
        // Prior cumulative stages cannot suppress a later frame with missing,
        // empty or incomplete worker diagnostics, including after a barrier.
        const auto generation = presence >= 3 ? 2u : 1u;
        const auto frameId = presence >= 3 ? 1u : 2u;
        if (presence >= 3) {
            require(co_await session.forward(endpoints[3], configuration(4, generation)));
            require(bool(co_await endpoints[2]->receive({})));
        }
        auto frame = video(5, frameId, generation);
        if (presence % 3 == 1) {
            (void)frame.mutable_video_frame()->mutable_stage_timing();
        } else if (presence % 3 == 2) {
            frame.mutable_video_frame()->mutable_stage_timing()->set_worker_queue_microseconds(0);
        }
        require(co_await session.forward(endpoints[3], frame));
        require(bool(co_await endpoints[2]->receive({})));
        auto dynamic = refinement(6, frameId);
        dynamic.set_generation(generation);
        dynamic.mutable_video_refinement()->set_kind(wire::DYNAMIC_DETAIL);
        *dynamic.mutable_video_refinement()->mutable_stage_timing() = workerStages();
        require(co_await session.forward(endpoints[3], dynamic));
        auto delivered = co_await endpoints[2]->receive({});
        require(delivered && delivered->bytes() == *protocol::encodeMessage(dynamic));
        delivered.reset();
        const auto& stages = session.diagnostics().pipeline.stages;
        for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
            require(stages[index].summary().samples == (presence % 3 == 2 && index == 0 ? 3u : 2u));
        }
        require(stages[0].summary().meanMicroseconds == 0 && budget->used == 0);
    }
}
ruvia::task<void> exerciseRefinementCleanup(ruvia::execution_handle worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    for (const auto status : {wire::DesktopRefinementAck::APPLIED, wire::DesktopRefinementAck::SUPERSEDED}) {
        for (const auto contract : {2u, 0u, 3u}) {
            server::DesktopSession session(worker, budget);
            auto endpoints = co_await pair(session);
            co_await requestOptimization(session, endpoints);
            require(co_await session.forward(endpoints[3], configuration()));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(3, 1)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], refinement()));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(5, 2))); // Submission retired; cleanup is still outstanding.
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], configuration(6, 2, contract)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(7, 1, 2)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], configuration(8, 3, contract)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(9, 1, 3)));
            require(bool(co_await endpoints[2]->receive({})));
            auto ack = refinementAck();
            ack.mutable_desktop_refinement_ack()->set_status(status);
            const auto beforeCleanup = session.diagnostics().forwardedMessages;
            require(co_await session.forward(endpoints[0], ack));
            auto delivered = co_await endpoints[1]->receive({});
            require(delivered && delivered->bytes() == *protocol::encodeMessage(ack)); // Old generation routed unchanged, once.
            delivered.reset();
            require(session.diagnostics().forwardedMessages == beforeCleanup + 1);
            require(session.diagnostics().feedbackMessages == 0 && session.diagnostics().videoGeneration == 3 && session.diagnostics().latestVideoFrame == 1);
            auto detail = refinement(10, 1, contract == 2 ? 1 : 0);
            detail.set_generation(3);
            require(co_await session.forward(endpoints[3], detail)); // Only real cleanup reopens the single slot.
            require(bool(co_await endpoints[2]->receive({})));
            const auto beforeDuplicate = session.diagnostics().forwardedMessages;
            ack.set_sequence(4);
            require(co_await session.forward(endpoints[0], ack));
            require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == beforeDuplicate);
            require(co_await session.forward(endpoints[3], video(11, 2, 3)));
            require(bool(co_await endpoints[2]->receive({})));
            if (contract == 2) {
                auto newAck = refinementAck(5);
                newAck.set_generation(3);
                require(co_await session.forward(endpoints[0], newAck)); // Duplicate old ACK did not complete the new lease.
                delivered = co_await endpoints[1]->receive({});
                require(delivered && delivered->bytes() == *protocol::encodeMessage(newAck));
                delivered.reset();
            }
            require(session.diagnostics().feedbackMessages == 0 && session.diagnostics().latestVideoFrame == 2);
        }
    }
    // No config (including fallback/high-version configs) may create another
    // auxiliary batch while the old lease is unacknowledged. Future ACKs cannot
    // impersonate the old tuple, even when generation-local IDs are identical.
    for (const auto contract : {2u, 0u, 3u}) {
        for (bool futureAck : {false, true}) {
            server::DesktopSession session(worker, budget);
            auto endpoints = co_await pair(session);
            co_await requestOptimization(session, endpoints);
            require(co_await session.forward(endpoints[3], configuration()));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(3, 1)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], refinement()));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], configuration(5, 2, contract)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], video(6, 1, 2)));
            require(bool(co_await endpoints[2]->receive({})));
            const auto messages = session.diagnostics().forwardedMessages;
            if (futureAck) {
                auto ack = refinementAck();
                ack.set_generation(2);
                require(!(co_await session.forward(endpoints[0], ack)));
            } else {
                auto detail = refinement(7, 1, contract == 2 ? 1 : 0);
                detail.set_generation(2);
                require(!(co_await session.forward(endpoints[3], detail)));
            }
            require(session.closeReason() == server::DesktopCloseReason::kInvalidFrameState);
            require(session.diagnostics().forwardedMessages == messages && session.diagnostics().feedbackMessages == 0 && budget->used == 0);
        }
    }
    // Timeout preserves the old-generation cleanup route and does not reset its
    // deadline when configs change. An unfinished PNG lease still blocks a new
    // batch, but ordinary video keeps flowing until cleanup arrives.
    for (bool cleanup : {false, true}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], refinement()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], configuration(5, 2)));
        require(bool(co_await endpoints[2]->receive({})));
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(2100));
        require(co_await session.forward(endpoints[3], video(6, 1, 2)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], configuration(7, 3)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(8, 1, 3)));
        require(bool(co_await endpoints[2]->receive({})));
        if (cleanup) {
            auto ack = refinementAck();
            ack.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::SUPERSEDED);
            const auto messages = session.diagnostics().forwardedMessages;
            require(co_await session.forward(endpoints[0], ack));
            auto delivered = co_await endpoints[1]->receive({});
            require(delivered && delivered->bytes() == *protocol::encodeMessage(ack));
            delivered.reset();
            require(session.diagnostics().forwardedMessages == messages + 1);
            auto detail = refinement(9);
            detail.set_generation(3);
            require(co_await session.forward(endpoints[3], detail));
            require(bool(co_await endpoints[2]->receive({})));
            ack.set_sequence(4);
            const auto beforeDuplicate = session.diagnostics().forwardedMessages;
            require(co_await session.forward(endpoints[0], ack));
            require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == beforeDuplicate);
        } else {
            auto detail = refinement(9);
            detail.set_generation(3);
            require(!(co_await session.forward(endpoints[3], detail))); // Timeout/config did not end the Browser lease.
        }
        require(session.diagnostics().feedbackMessages == 0 && budget->used == 0);
    }
}
ruvia::task<void> exerciseOptimization(ruvia::execution_handle worker) {
    auto budget = std::make_shared<server::DesktopBudget>(256 * 1024 * 1024);
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session, false); // Diagnostic ACKs do not require write control.
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
        require(co_await session.forward(endpoints[3], refinement()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(5, 2))); // Video never waits for PNG ACK.
        require(bool(co_await endpoints[2]->receive({})));
        auto ack = refinementAck();
        ack.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::SUPERSEDED);
        require(co_await session.forward(endpoints[0], ack)); // Retired base != latest frame is valid.
        delivered = co_await endpoints[1]->receive({});
        auto decoded = protocol::decodeMessage(delivered->bytes());
        require(decoded && decoded->has_desktop_refinement_ack() && !decoded->has_quality_feedback());
        delivered.reset();
        const auto messages = session.diagnostics().forwardedMessages;
        ack.set_sequence(4);
        require(co_await session.forward(endpoints[0], ack));
        require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == messages && session.diagnostics().feedbackMessages == 0);
        auto feedback = refresh(5);
        auto* quality = feedback.mutable_quality_feedback();
        quality->set_displayed_generation(1);
        quality->set_displayed_frame(2);
        quality->set_decode_microseconds(123);
        quality->set_queue_depth(3);
        quality->mutable_browser_timing()->set_decode_microseconds(0);
        quality->mutable_browser_timing()->set_canvas_submit_microseconds(30);
        require(co_await session.forward(endpoints[0], feedback));
        delivered = co_await endpoints[1]->receive({});
        decoded = protocol::decodeMessage(delivered->bytes());
        require(decoded && !decoded->quality_feedback().has_browser_timing() && decoded->quality_feedback().decode_microseconds() == 123 && decoded->quality_feedback().queue_depth() == 3);
        delivered.reset();
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kBrowserDecode)].summary().samples == 1);
        feedback.set_sequence(6);
        quality->set_displayed_frame(3); // Unknown future canvas, stripped but unobserved.
        require(co_await session.forward(endpoints[0], feedback));
        require(bool(co_await endpoints[1]->receive({})));
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kBrowserDecode)].summary().samples == 1);
        require(co_await session.forward(endpoints[3], configuration(6, 2)));
        require(bool(co_await endpoints[2]->receive({})));
        ack.set_sequence(7); // Old generation duplicate is still a no-op.
        require(co_await session.forward(endpoints[0], ack));
        require(session.ready() && budget->used == 0);
        session.logHealth();
    }
    // Unknown higher versions and either missing declaration retain single-batch legacy.
    for (const auto contracts : std::array<std::array<std::uint32_t, 2>, 5>{{{0, 2}, {1, 2}, {3, 2}, {2, 0}, {2, 3}}}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints, contracts[0]);
        require(co_await session.forward(endpoints[1], pipelineReport()));
        require(co_await session.forward(endpoints[3], configuration(2, 1, contracts[1])));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], refinement(4, 1, 0)));
        require(bool(co_await endpoints[2]->receive({})));
        require(session.diagnostics().pipeline.stages[static_cast<std::size_t>(server::DesktopStage::kRelayNativeWrite)].summary().samples == 0);
        for (std::size_t index = 0; index <= static_cast<std::size_t>(server::DesktopStage::kVideoEncode); ++index) {
            require(session.diagnostics().pipeline.stages[index].summary().samples == 0);
        }
        require(!(co_await session.forward(endpoints[3], refinement(5, 1, 0))));
    }
    for (int diagnostic = 0; diagnostic < 4; ++diagnostic) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        if (diagnostic == 0) {
            require(!(co_await session.forward(endpoints[0], refinementAck(2))));
        } else if (diagnostic == 1) {
            require(co_await session.forward(endpoints[1], pipelineReport()));
            require(session.ready() && budget->used == 0 && session.diagnostics().pipeline.dropped == 1);
        } else if (diagnostic == 2) {
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
        require(co_await session.forward(endpoints[3], video(4, 1, nextGeneration ? 2 : 1)));
        require(bool(co_await endpoints[2]->receive({})));
        auto detail = refinement(5);
        detail.set_generation(nextGeneration ? 2 : 1);
        require((co_await session.forward(endpoints[3], detail)) == nextGeneration);
        if (nextGeneration) {
            require(bool(co_await endpoints[2]->receive({})));
        }
    }
    // Bad correlation and wrong ingress directions cannot acquire credit.
    for (int invalid = 0; invalid < 9; ++invalid) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], refinement()));
        require(bool(co_await endpoints[2]->receive({})));
        auto message = refinementAck();
        auto sender = endpoints[0];
        switch (invalid) {
        case 0:
            message.set_frame_id(2);
            break;
        case 1:
            message.mutable_desktop_refinement_ack()->set_batch_id(2);
            break;
        case 2:
            message.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::UNSPECIFIED);
            break;
        case 3:
            sender = endpoints[1];
            break;
        case 4:
            message.set_channel_id("video");
            sender = endpoints[2];
            break;
        case 5:
            message = pipelineReport(3);
            break; // Browser may never report Agent stages.
        case 6:
            message = pipelineReport(5);
            message.set_channel_id("video");
            sender = endpoints[3];
            break;
        case 7:
            message = pipelineReport(2);
            sender = endpoints[1];
            for (int index = 0; index < 32; ++index) {
                *message.mutable_desktop_pipeline_report()->add_samples() = message.desktop_pipeline_report().samples(0);
            }
            break;
        case 8:
            message = refresh(3);
            message.mutable_quality_feedback()->set_displayed_generation(1);
            message.mutable_quality_feedback()->set_displayed_frame(1);
            message.mutable_quality_feedback()->mutable_browser_timing()->set_decode_microseconds(60000001);
            break;
        }
        require(!(co_await session.forward(sender, message)) && session.closed());
        require(session.diagnostics().feedbackMessages == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        for (std::uint64_t batch = 1; batch <= 130; ++batch) {
            require(co_await session.forward(endpoints[3], video(batch * 2 + 1, batch)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[3], refinement(batch * 2 + 2, batch, batch)));
            require(bool(co_await endpoints[2]->receive({})));
            require(co_await session.forward(endpoints[0], refinementAck(batch + 2, batch, batch)));
            require(bool(co_await endpoints[1]->receive({})));
        }
        const auto messages = session.diagnostics().forwardedMessages;
        require(co_await session.forward(endpoints[0], refinementAck(133))); // Older than bounded exact tombstones.
        require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == messages);
        require(session.diagnostics().feedbackMessages == 0);
    }
    {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        co_await requestOptimization(session, endpoints);
        require(co_await session.forward(endpoints[3], configuration()));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], video(3, 1)));
        require(bool(co_await endpoints[2]->receive({})));
        require(co_await session.forward(endpoints[3], refinement()));
        require(bool(co_await endpoints[2]->receive({})));
        co_await ruvia::sleep_for(worker, std::chrono::milliseconds(2100));
        const auto messages = session.diagnostics().forwardedMessages;
        require(co_await session.forward(endpoints[0], refinementAck()));
        auto cleanup = co_await endpoints[1]->receive({});
        require(cleanup && cleanup->bytes() == *protocol::encodeMessage(refinementAck()));
        cleanup.reset();
        require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == messages + 1);
        require(co_await session.forward(endpoints[0], refinementAck(4)));
        require(session.ready() && budget->used == 0 && session.diagnostics().forwardedMessages == messages + 1);
        require(co_await session.forward(endpoints[3], video(5, 2))); // Timeout never pauses normal video.
        require(bool(co_await endpoints[2]->receive({})));
        require(!(co_await session.forward(endpoints[3], refinement(6, 2, 2)))); // Cleanup does not unpause the timed-out generation.
        require(session.closeReason() == server::DesktopCloseReason::kInvalidFrameState && session.diagnostics().feedbackMessages == 0);
    }
    require(budget->used == 0);
}
ruvia::task<void> exercise(ruvia::execution_handle worker) {
    co_await exerciseWriters(worker);
    co_await exerciseDynamicStagePresence(worker);
    co_await exerciseOptimization(worker);
    co_await exerciseRefinementCleanup(worker);
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
        const auto occupied = budget->used;
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
        ruvia::execution_domain ingress(worker.io_context());
        co_await ruvia::with_task_scope(worker, [&](ruvia::task_scope& scope) -> ruvia::task<void> {
            scope.spawn(ruvia::on(ingress.handle(), forwardBurst(session, endpoints[0], ingress.handle(), finished)));
            co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            require(session.ready() && !finished);
            require(budget->used == 9 * protocol::encodeMessage(refresh(2))->size());
            for (std::uint64_t sequence = 2; sequence <= 100; ++sequence) {
                auto delivery = co_await endpoints[1]->receive({});
                require(delivery && delivery->bytes() == *protocol::encodeMessage(refresh(sequence)));
            }
            co_await scope.join();
            require(finished && session.ready() && budget->used == 0);
            require(session.diagnostics().backpressureEvents > 0);
            require(session.diagnostics().forwardedMessages == 99);
        });
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
        co_await ruvia::with_task_scope(worker, [&](ruvia::task_scope& scope) -> ruvia::task<void> {
            scope.spawn(forwardVideoBurst(session, endpoints[3], configuration, finished));
            co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
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
                co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            }
            co_await scope.join();
            require(finished && session.ready() && session.diagnostics().videoFrames == 30 && session.diagnostics().backpressureEvents > 0 && budget->used == 0);
            const auto& stages = session.diagnostics().pipeline.stages;
            require(stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoAdmission)].summary().maximumMicroseconds >= 8000);
            require(stages[static_cast<std::size_t>(server::DesktopStage::kServerVideoWsWrite)].summary().samples == 0); // Receiving is not a completed WS write.
        });
    }
    for (bool cancel : {false, true}) {
        server::DesktopSession session(worker, budget);
        auto endpoints = co_await pair(session);
        for (std::uint64_t sequence = 2; sequence < 10; ++sequence) {
            require(co_await session.forward(endpoints[0], refresh(sequence)));
        }
        bool finished = false;
        co_await ruvia::with_task_scope(worker, [&](ruvia::task_scope& scope) -> ruvia::task<void> {
            ruvia::stop_source stop;
            scope.spawn(awaitRejected(session, endpoints[0], stop.token(), finished));
            co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10));
            require(!finished && session.ready());
            if (cancel) {
                stop.request_stop();
            } else {
                session.close();
            }
            co_await scope.join();
            require(finished && !session.ready());
            require(!(co_await endpoints[1]->receive({})));
            require(budget->used == 0);
        });
    }
    require(budget->used == 0);
    {
        auto session = std::make_unique<server::DesktopSession>(worker, budget);
        auto endpoints = co_await pair(*session);
        std::array<bool, 4> finished{};
        co_await ruvia::with_task_scope(worker, [&](ruvia::task_scope& scope) -> ruvia::task<void> {
            for (std::size_t i = 0; i < endpoints.size(); ++i) {
                scope.spawn(awaitClose(*endpoints[i], finished[i]));
            }
            co_await ruvia::sleep_for(worker, std::chrono::milliseconds(1));
            session.reset(); // Destruction must wake all four writers while endpoint handles survive.
            co_await scope.join();
            for (bool complete : finished) {
                require(complete);
            }
        });
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
        quality->set_video_refinement(true);
        auto* report = quality->mutable_visible_frame_timing();
        report->set_sample_count(48);
        report->set_p95_microseconds(258000);
        report->set_p99_microseconds(300000);
        report->set_long_gap_count(5);
        report->set_observed_fps_milli(20000);
        require(co_await session.forward(endpoints[0], feedback));
        received = co_await endpoints[1]->receive({});
        const auto forwarded = protocol::decodeMessage(received->bytes());
        require(forwarded && !forwarded->quality_feedback().has_visible_frame_timing() && forwarded->quality_feedback().video_refinement());
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
} // namespace
int main() {
    try {
        ruvia::io_runtime pool({.thread_count_ = 1});
        pool.start();
        auto task = pool.control().start(exercise(pool.control()));
        task.get();
        pool.join();
        std::cout << "Desktop four-endpoint pairing, binding, FIFO, replay, read-only access and bounded in-flight ownership passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
