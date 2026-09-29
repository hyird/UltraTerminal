#include "relay.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <exception>
#include <stdexcept>
#include <utility>

#include <ruvia/core/AsioTask.h>

#include "continuity.protocol.h"
#include "desktop.transport.h"
#include "platform/windows/desktop_failure.error.h"
#include "send_queue.runtime.h"

namespace ultraterimal::agent {
struct DesktopWorkerChannel::State {
    DesktopRelay::State& relay;
    DesktopSendQueue input;
    bool cancelled = false;
    State(DesktopRelay::State& owner, ruvia::WorkerHandle worker) : relay(owner), input(std::move(worker), 64, 2 * 1024 * 1024) {}
};
struct DesktopRelay::State {
    ruvia::EventLoop loop;
    DesktopTransport& transport;
    DesktopSendQueue control, video;
    DesktopContinuity continuity;
    std::shared_ptr<DesktopWorkerChannel> worker;
    std::exception_ptr failure;
    wire::Envelope terminal;
    bool stopped = false;
    State(ruvia::EventLoop owner, DesktopTransport& remote)
        : loop(owner), transport(remote), control(owner.handle(), 32, 2 * 1024 * 1024), video(owner.handle(), 32, 32 * 1024 * 1024) {}
    void detach() {
        if (worker) {
            worker->cancel();
            worker.reset();
        }
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
    asio::awaitable<void> write(DesktopSendQueue& queue, bool isVideo) {
        try {
            while (auto message = co_await ruvia::asAwaitable(queue.receive())) {
                const auto bytes = message->ByteSizeLong();
                try {
                    if (isVideo) {
                        co_await transport.sendVideo(std::move(*message));
                    } else {
                        co_await transport.sendCursor(std::move(*message));
                    }
                } catch (...) {
                    queue.complete(bytes);
                    throw;
                }
                queue.complete(bytes);
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
                auto target = worker;
                if (!target || target->cancelled() || !continuity.incoming(message)) {
                    continue;
                }
                if (!(co_await ruvia::asAwaitable(target->state_->input.send(std::move(message)))) && !target->cancelled()) {
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
DesktopRelay::DesktopRelay(ruvia::EventLoop loop, DesktopTransport& transport) : state_(std::make_unique<State>(std::move(loop), transport)) {}
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
    state_->worker = std::shared_ptr<DesktopWorkerChannel>(new DesktopWorkerChannel(std::make_unique<DesktopWorkerChannel::State>(*state_, state_->loop.handle())));
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
asio::awaitable<void> DesktopWorkerChannel::sendVideo(wire::Envelope message) {
    if (state_->cancelled || !state_->relay.continuity.outgoing(message) || !(co_await ruvia::asAwaitable(state_->relay.video.send(std::move(message))))) {
        throw std::runtime_error("Desktop video relay stopped or exhausted its namespace");
    }
}
asio::awaitable<void> DesktopWorkerChannel::sendCursor(wire::Envelope message) {
    if (state_->cancelled || !state_->relay.continuity.outgoing(message) || !(co_await ruvia::asAwaitable(state_->relay.control.send(std::move(message))))) {
        throw std::runtime_error("Desktop control relay stopped or exhausted its namespace");
    }
}
asio::awaitable<wire::Envelope> DesktopWorkerChannel::receiveInput() {
    auto message = co_await ruvia::asAwaitable(state_->input.receive());
    if (!message) {
        throw std::runtime_error("Desktop worker input detached");
    }
    co_return std::move(*message);
}
void DesktopWorkerChannel::completeInput(std::size_t bytes) { state_->input.complete(bytes); }
} // namespace ultraterimal::agent
