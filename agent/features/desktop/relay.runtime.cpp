#include "relay.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio_bridge.h>
#include <event_loop.h>
#include <exception>
#include <stdexcept>
#include <utility>

#include "continuity.protocol.h"
#include "desktop.transport.h"
#include "pipeline.runtime.h"
#include "platform/windows/desktop_failure.error.h"
#include "send_queue.runtime.h"

namespace ultraterimal::agent {
struct DesktopWorkerChannel::State {
    DesktopRelay::State& relay;
    DesktopSendQueue input;
    bool cancelled = false;
    State(DesktopRelay::State& owner, ruvia::event_loop worker) : relay(owner), input(worker.worker(), 64, 2 * 1024 * 1024) {}
};
struct DesktopRelay::State {
    ruvia::event_loop loop;
    DesktopTransport& transport;
    DesktopSendQueue control, video;
    DesktopContinuity continuity;
    DesktopPipelineBuffer pipeline;
    bool pipelineEnabled = false;
    std::shared_ptr<DesktopWorkerChannel> worker;
    std::exception_ptr failure;
    wire::Envelope terminal;
    bool stopped = false;
    bool audioEnabled = false;
    State(ruvia::event_loop owner, DesktopTransport& remote)
        : loop(owner), transport(remote), control(owner.worker(), 32, 2 * 1024 * 1024), video(owner.worker(), 2, 8 * 1024 * 1024) {}
    void detach() {
        if (worker) {
            worker->cancel();
            worker.reset();
        }
        pipelineEnabled = false;
        pipeline.clear();
    }
    void stop() {
        stopped = true;
        detach();
        control.close();
        video.close();
        transport.cancel();
    }
    void fail(wire::DesktopFault::Stage stage) {
        if (!stopped && !failure) {
            failure = std::make_exception_ptr(DesktopFailure(desktopFailureCode(std::current_exception(), stage), "Native desktop channel failed", stage));
        }
        stop();
    }
    void flushPipeline() {
        if (!pipelineEnabled) {
            return;
        }
        const auto now = DesktopPipelineBuffer::Clock::now();
        if (auto report = pipeline.pending(now); report && control.trySend(std::move(*report))) {
            pipeline.sent(now);
        }
    }
    asio::awaitable<void> write(DesktopSendQueue& queue, bool isVideo) {
        try {
            while (auto entry = co_await ruvia::as_awaitable(queue.receive())) {
                auto sample = isVideo && entry->samplePipeline ? DesktopPipelineBuffer::sample(entry->message, wire::DesktopPipelineSample::AGENT_RELAY) : std::nullopt;
                const auto started = DesktopPipelineBuffer::Clock::now();
                if (sample) {
                    // Includes bounded admission backpressure and queue dwell,
                    // both observed on this owner loop, not a remote clock.
                    sample->set_send_queue_wait_microseconds(DesktopPipelineBuffer::duration(started - entry->receivedAt));
                }
                const bool diagnostic = entry->message.has_desktop_pipeline_report();
                try {
                    if (isVideo) {
                        const auto elapsed = co_await transport.sendVideo(std::move(entry->message), entry->optimizationContract);
                        if (sample) {
                            sample->set_native_write_microseconds(DesktopPipelineBuffer::duration(elapsed));
                        }
                    } else {
                        co_await transport.sendCursor(std::move(entry->message));
                    }
                } catch (...) {
                    queue.complete(*entry);
                    throw;
                }
                if (sample && pipelineEnabled) {
                    pipeline.add(std::move(*sample));
                }
                queue.complete(*entry);
                if (!diagnostic) {
                    flushPipeline();
                }
            }
        } catch (...) {
            fail(isVideo ? wire::DesktopFault::BRIDGE_VIDEO_SEND : wire::DesktopFault::BRIDGE_CONTROL_SEND);
        }
    }
    asio::awaitable<void> read() {
        try {
            while (!stopped) {
                auto message = co_await transport.receiveInput();
                if (message.has_session_close() || message.has_session_error()) {
                    terminal = std::move(message);
                    stop();
                    co_return;
                }
                // Capture this worker before suspending on its bounded queue.
                // Replacement closes that queue, never replays it into a new one.
                if (message.has_audio_control()) {
                    if (!transport.serverDesktopAudio()) {
                        throw std::runtime_error("Desktop audio is not supported by server");
                    }
                    audioEnabled = message.audio_control().enabled();
                }
                auto target = worker;
                if (!target || target->cancelled() || !continuity.incoming(message)) {
                    continue;
                }
                if (!(co_await ruvia::as_awaitable(target->state_->input.send(std::move(message)))) && !target->cancelled()) {
                    throw std::runtime_error("Desktop input queue rejected command");
                }
            }
        } catch (...) {
            fail(wire::DesktopFault::BRIDGE_INPUT_RECEIVE);
        }
    }
    asio::awaitable<void> watch() {
        try {
            co_await transport.watchVideo();
        } catch (...) {
            fail(wire::DesktopFault::BRIDGE_VIDEO_RECEIVE);
        }
    }
};
DesktopRelay::DesktopRelay(ruvia::event_loop loop, DesktopTransport& transport) : state_(std::make_unique<State>(std::move(loop), transport)) {}
DesktopRelay::~DesktopRelay() { stop(); }
asio::awaitable<void> DesktopRelay::run() {
    using namespace asio::experimental::awaitable_operators;
    co_await (state_->write(state_->control, false) && state_->write(state_->video, true) && state_->read() && state_->watch());
    if (state_->failure) {
        std::rethrow_exception(state_->failure);
    }
}
std::shared_ptr<DesktopWorkerChannel> DesktopRelay::attach() {
    if (state_->stopped || state_->worker) {
        throw std::logic_error("Desktop relay cannot attach worker");
    }
    state_->continuity.beginWorker();
    state_->worker = std::shared_ptr<DesktopWorkerChannel>(new DesktopWorkerChannel(std::make_unique<DesktopWorkerChannel::State>(*state_, state_->loop)));
    return state_->worker;
}
void DesktopRelay::detach() { state_->detach(); }
void DesktopRelay::stop() { state_->stop(); }
wire::Envelope DesktopRelay::terminal() const { return state_->terminal; }
DesktopWorkerChannel::DesktopWorkerChannel(std::unique_ptr<State> state) : state_(std::move(state)) {}
DesktopWorkerChannel::~DesktopWorkerChannel() = default;
void DesktopWorkerChannel::cancel() {
    state_->cancelled = true;
    state_->input.close();
}
bool DesktopWorkerChannel::cancelled() const { return state_->cancelled; }
asio::awaitable<void> DesktopWorkerChannel::sendVideo(wire::Envelope message, std::uint32_t allowedOptimizationContract) {
    auto& relay = state_->relay;
    if (state_->cancelled || !relay.continuity.outgoing(message)) {
        throw std::runtime_error("Desktop video relay stopped or exhausted its namespace");
    }
    if (message.has_video_config()) {
        relay.pipelineEnabled = relay.transport.serverOptimizationContract() == 2 && allowedOptimizationContract == 2 && message.video_config().desktop_optimization_contract() == 2;
        if (!relay.pipelineEnabled) {
            relay.pipeline.clear();
        }
    }
    if (!(co_await ruvia::as_awaitable(relay.video.send(std::move(message), relay.pipelineEnabled, allowedOptimizationContract)))) {
        throw std::runtime_error("Desktop video relay stopped or exhausted its namespace");
    }
}
asio::awaitable<void> DesktopWorkerChannel::sendCursor(wire::Envelope message) {
    auto& relay = state_->relay;
    if (state_->cancelled || !relay.continuity.outgoing(message)) {
        throw std::runtime_error("Desktop control relay stopped or exhausted its namespace");
    }
    if (message.has_desktop_pipeline_report()) {
        if (relay.pipelineEnabled) {
            relay.pipeline.merge(message.desktop_pipeline_report());
            relay.flushPipeline();
        }
        co_return; // Never await diagnostics queue capacity on the pipe reader.
    }
    if (!(co_await ruvia::as_awaitable(relay.control.send(std::move(message))))) {
        throw std::runtime_error("Desktop control relay stopped or exhausted its namespace");
    }
}
asio::awaitable<void> DesktopWorkerChannel::sendAudio(wire::Envelope message) {
    auto& relay = state_->relay;
    if (state_->cancelled || !relay.continuity.outgoing(message)) {
        throw std::runtime_error("Desktop audio relay stopped or exhausted its namespace");
    }
    if (message.has_audio_frame()) {
        if (relay.control.pendingCount() >= 16) {
            co_return;
        }
        (void)relay.control.trySend(std::move(message));
        co_return;
    }
    if (!(co_await ruvia::as_awaitable(relay.control.send(std::move(message))))) {
        throw std::runtime_error("Desktop control relay stopped or exhausted its namespace");
    }
}
asio::awaitable<DesktopSendQueue::Entry> DesktopWorkerChannel::receiveInput() {
    auto message = co_await ruvia::as_awaitable(state_->input.receive());
    if (!message) {
        throw std::runtime_error("Desktop worker input detached");
    }
    co_return std::move(*message);
}
void DesktopWorkerChannel::completeInput(DesktopSendQueue::Entry& entry) { state_->input.complete(entry); }
std::uint32_t DesktopWorkerChannel::serverOptimizationContract() const { return state_->relay.transport.serverOptimizationContract(); }
bool DesktopWorkerChannel::serverContinuousFrames() const { return state_->relay.transport.serverContinuousFrames(); }
bool DesktopWorkerChannel::serverDesktopAudio() const { return state_->relay.transport.serverDesktopAudio(); }
bool DesktopWorkerChannel::audioEnabled() const { return state_->relay.audioEnabled; }
} // namespace ultraterimal::agent
