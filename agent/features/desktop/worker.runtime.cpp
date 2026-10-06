#include "worker.runtime.h"

#include <algorithm>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "optimization.runtime.h"
#include "pipeline.runtime.h"
#include "platform/windows/desktop_failure.error.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/input.transport.h"
#include "platform/windows/pipe.transport.h"
#include "ultraterimal/protocol/desktop_failure.protocol.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"
#include "video.runtime.h"
#include "worker_output.runtime.h"

namespace ultraterimal::agent {
namespace {
class DesktopWorker {
  public:
    DesktopWorker(ruvia::execution_handle loop, NamedPipeTransport& pipe, std::uint32_t windowsSession, const wire::Envelope& start)
        : pipe_(pipe),
          probeTimer_(loop.executor()),
          video_(std::move(loop), start.desktop_start().display_index(), start.desktop_start().desktop_optimization_contract()),
          incoming_({start.session_id(), windowsSession, start.desktop_start().write_control(), protocol::DesktopPipeSource::kService}),
          outgoing_({start.session_id(), windowsSession, start.desktop_start().write_control(), protocol::DesktopPipeSource::kDesktop}),
          session_(start.session_id()), writeControl_(start.desktop_start().write_control()),
          optimization_(start.desktop_start().desktop_optimization_contract()) {
        // Both IPC video validators begin with allow0. Start carries Server
        // permission only; it is not a Browser request and cannot activate v2.
        if (!incoming_.accept(start)) {
            throw DesktopFailure(wire::DesktopFault::PROTOCOL_REJECTED, "Desktop startup binding rejected");
        }
        // The constructor's immutable Server support enables the first Config2
        // declaration; initial joint permission0 must not erase that support.
        video_.setOptimizationContract(optimization_.allowedContract());
    }
    ~DesktopWorker() { stop(); }
    void stop() {
        stopped_ = true;
        probeTimer_.cancel();
        video_.stop();
        pipe_.cancel();
        if (input_) {
            try {
                input_->releaseAll();
            } catch (...) {
                releaseFailure_ = std::current_exception();
            }
            input_.reset();
        }
    }
    asio::awaitable<void> send(wire::Envelope message) {
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop IPC sequence exhausted");
        }
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id(session_);
        message.set_sequence(++sequence_);
        if (!outgoing_.accept(message)) {
            throw std::runtime_error("Desktop output violates IPC contract");
        }
        if (message.has_video_config()) {
            optimization_.configure(message.video_config());
            pipelineEnabled_ = optimization_.activeContract() == 2;
            if (!pipelineEnabled_) {
                pipeline_.clear();
            }
        }
        auto bytes = protocol::encodeMessage(message);
        if (!bytes) {
            throw std::runtime_error("Cannot encode desktop IPC message");
        }
        auto sample = pipelineEnabled_ ? DesktopPipelineBuffer::sample(message, wire::DesktopPipelineSample::DESKTOP_WORKER) : std::nullopt;
        const auto started = DesktopPipelineBuffer::Clock::now();
        co_await pipe_.send(std::move(*bytes));
        if (sample) {
            sample->set_ipc_write_microseconds(DesktopPipelineBuffer::duration(DesktopPipelineBuffer::Clock::now() - started));
            pipeline_.add(std::move(*sample));
        }
    }
    asio::awaitable<void> flushPipeline() {
        const auto now = DesktopPipelineBuffer::Clock::now();
        if (pipelineEnabled_) {
            if (auto report = pipeline_.pending(now)) {
                pipeline_.sent(now);
                co_await send(std::move(*report));
            }
        }
    }
    asio::awaitable<void> write() {
        const char* stage = "video_start";
        try {
            while (!stopped_) {
                stage = "control_ipc_write";
                if (secureAttention_) {
                    auto request = std::exchange(secureAttention_, std::nullopt);
                    if (request->generation() == inputGeneration_ && !resizing_) {
                        co_await send(std::move(*request));
                    }
                }
                if (ready_ && std::chrono::steady_clock::now() >= nextProbe_) {
                    if (probeNonce_ == std::numeric_limits<std::uint32_t>::max()) {
                        throw std::overflow_error("Desktop probe sequence exhausted");
                    }
                    wire::Envelope probe;
                    probe.set_channel_id("input");
                    probe.mutable_desktop_probe()->set_nonce(++probeNonce_);
                    probe.mutable_desktop_probe()->set_accepted_response_nonce(lastAcceptedResponseNonce_);
                    const auto sentAt = std::chrono::steady_clock::now();
                    outstandingProbes_.push_back({probeNonce_, sentAt});
                    if (outstandingProbes_.size() > 32) {
                        outstandingProbes_.pop_front();
                    }
                    nextProbe_ = sentAt + std::chrono::seconds(2);
                    co_await send(std::move(probe));
                }
                if (resize_) {
                    const auto request = std::exchange(resize_, std::nullopt);
                    resizing_ = true;
                    stage = "input_release";
                    if (input_) {
                        input_->releaseAll();
                    }
                    stage = "display_resize";
                    const bool applied = co_await video_.resize(request->width(), request->height());
                    if (stopped_) {
                        break;
                    }
                    if (applied) {
                        input_.reset();
                        refresh_ = true;
                    }
                    resizing_ = false;
                    stage = "control_ipc_write";
                    wire::Envelope result;
                    result.set_channel_id("input");
                    result.mutable_desktop_resize_result()->set_request_id(request->request_id());
                    result.mutable_desktop_resize_result()->set_applied(applied);
                    co_await send(std::move(result));
                }
                const bool refresh = std::exchange(refresh_, false);
                stage = "capture_encode";
                auto update = co_await video_.next(refresh);
                if (stopped_) {
                    break;
                }
                if (!update) {
                    refresh_ = refresh_ || refresh;
                    co_await flushPipeline();
                    continue;
                }
                if (writeControl_ && update->configuration) {
                    stage = "input_release";
                    if (input_) {
                        input_->releaseAll();
                        input_.reset();
                    }
                    stage = "input_binding";
                    input_ = std::make_unique<DesktopInput>(update->desktopLeft, update->desktopTop,
                                                            update->configuration->width(), update->configuration->height());
                    inputGeneration_ = update->generation;
                    rebindRequested_ = false;
                }
                stage = "video_ipc_write";
                if (!ready_) {
                    if (update->frame.data().empty()) {
                        continue;
                    }
                    wire::Envelope ready;
                    ready.mutable_desktop_ready();
                    co_await send(std::move(ready));
                    lastAck_ = std::chrono::steady_clock::now();
                    ready_ = true;
                }
                if (update->configuration) {
                    // Preserve the capability declaration, including the first
                    // Config2. The validator's allow0 keeps it on the legacy path
                    // until an actual Browser request enables the NEXT barrier.
                    wire::Envelope configuration;
                    configuration.set_channel_id("video");
                    configuration.set_generation(update->generation);
                    *configuration.mutable_video_config() = std::move(*update->configuration);
                    co_await send(std::move(configuration));
                }
                if (update->cursor) {
                    wire::Envelope cursor;
                    cursor.set_channel_id("input");
                    *cursor.mutable_cursor_update() = std::move(*update->cursor);
                    co_await send(std::move(cursor));
                }
                const bool noVideoFrame = update->frame.data().empty();
                co_await writeDesktopPixels(*update, [this](wire::Envelope message) { return send(std::move(message)); });
                if (noVideoFrame) {
                    refresh_ = refresh_ || refresh;
                }
                co_await flushPipeline();
            }
        } catch (...) {
            rememberFailure(stage);
            stop();
            throw;
        }
    }
    void checkLiveness(std::chrono::steady_clock::time_point now) {
        if (ready_ && now - lastAck_ >= std::chrono::seconds(60)) {
            throw std::runtime_error("Desktop browser liveness expired");
        }
    }
    asio::awaitable<void> read() {
        const char* stage = "input_ipc_read";
        try {
            while (!stopped_) {
                stage = "input_ipc_read";
                auto bytes = co_await pipe_.receive();
                stage = "input_validation";
                auto message = protocol::decodeMessage(bytes);
                if (!message || !incoming_.accept(*message)) {
                    throw std::runtime_error("Desktop command violates IPC contract");
                }
                if (message->has_session_close() || message->has_session_error()) {
                    requestedClose_ = true;
                    stop();
                    co_return;
                }
                if (message->has_desktop_probe() || message->has_input_event()) {
                    // A command arriving at the lease boundary must not run before
                    // the periodic watchdog gets its next turn on this Worker.
                    checkLiveness(std::chrono::steady_clock::now());
                }
                if (message->has_desktop_probe()) {
                    const auto nonce = message->desktop_probe().nonce();
                    if (std::find(confirmedProbes_.begin(), confirmedProbes_.end(), nonce) != confirmedProbes_.end()) {
                        throw std::runtime_error("Replayed desktop probe response");
                    }
                    const auto now = std::chrono::steady_clock::now();
                    const auto probe = std::find_if(outstandingProbes_.begin(), outstandingProbes_.end(),
                                                    [nonce](const OutstandingProbe& item) { return item.nonce == nonce; });
                    if (probe == outstandingProbes_.end()) {
                        continue;
                    }
                    const auto sentAt = probe->sentAt;
                    outstandingProbes_.erase(probe);
                    confirmedProbes_.push_back(nonce);
                    if (confirmedProbes_.size() > 32) {
                        confirmedProbes_.pop_front();
                    }
                    if (now - sentAt >= std::chrono::seconds(60)) {
                        continue;
                    }
                    lastAck_ = now;
                    video_.observeRoundTrip(std::chrono::duration_cast<std::chrono::microseconds>(now - sentAt));
                    lastAcceptedResponseNonce_ = std::max(lastAcceptedResponseNonce_, nonce);
                } else if (message->has_desktop_refinement_ack()) {
                    video_.refinementAck(*message); // Independent of input binding and frame ACKs.
                } else if (message->has_input_event()) {
                    // Old coordinates can arrive while Windows changes the mode.
                    // Resume only after the new capture configuration binds input.
                    if (resizing_ || (ready_ && !input_)) {
                        continue;
                    }
                    if (!ready_ || !input_) {
                        throw std::runtime_error("Desktop input is not ready");
                    }
                    // Both channels are independent: bytes submitted before a
                    // video rebind may arrive after its new input adapter exists.
                    // The previous adapter has already released its held state.
                    if (message->generation() < inputGeneration_) {
                        continue;
                    }
                    if (message->generation() != inputGeneration_) {
                        throw std::runtime_error("Desktop input belongs to an unknown generation");
                    }
                    if (message->input_event().kind() == wire::InputEvent::SECURE_ATTENTION) {
                        stage = "input_release";
                        input_->releaseAll();
                        secureAttention_ = std::move(*message);
                        continue;
                    }
                    stage = "input_apply";
                    input_->apply(message->input_event());
                    if (input_->needsRebind() && !rebindRequested_) {
                        rebindRequested_ = true;
                        video_.invalidate();
                    }
                } else if (message->has_request_refresh()) {
                    refresh_ = true;
                } else if (message->has_quality_feedback()) {
                    video_.feedback(message->quality_feedback());
                } else if (message->has_desktop_video_capabilities()) {
                    auto capabilities = message->desktop_video_capabilities();
                    optimization_.request(capabilities.desktop_optimization_contract());
                    incoming_.setOptimizationContract(optimization_.allowedContract());
                    outgoing_.setOptimizationContract(optimization_.allowedContract());
                    capabilities.set_desktop_optimization_contract(optimization_.allowedContract());
                    // A's explicit gate receives FULL pending permission, not
                    // Server capability. Changed permission/capabilities retire
                    // stale results and activate only at a NEW config/generation.
                    video_.setOptimizationContract(optimization_.allowedContract());
                    video_.capabilities(capabilities);
                } else if (message->has_desktop_resize()) {
                    if (!ready_ || resize_ || resizing_) {
                        throw std::runtime_error("Desktop resize request queue exceeded");
                    }
                    resize_ = message->desktop_resize();
                } else {
                    throw std::runtime_error("Desktop command is not supported by this execution stage");
                }
            }
        } catch (...) {
            rememberFailure(stage);
            stop();
            throw;
        }
    }
    asio::awaitable<void> watch() {
        while (!stopped_) {
            probeTimer_.expires_after(std::chrono::milliseconds(100));
            std::error_code error;
            co_await probeTimer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (stopped_) {
                co_return;
            }
            const auto now = std::chrono::steady_clock::now();
            try {
                if (error) {
                    throw std::runtime_error("Desktop liveness timer failed");
                }
                checkLiveness(now);
            } catch (...) {
                rememberFailure("liveness");
                stop();
                throw;
            }
        }
    }
    asio::awaitable<std::uint32_t> run() {
        using namespace asio::experimental::awaitable_operators;
        try {
            co_await (write() && read() && watch());
        } catch (...) {
            if (failure_ || !requestedClose_) {
                // Parallel coroutine cancellation can mask the original error
                // with operation_aborted or multiple_exceptions. Preserve and
                // report the first fault, never a teardown side effect.
                reportFailure();
                const auto stage = desktopFailureStage(failureStage_);
                co_return protocol::desktopFailureExitCode(stage, desktopFailureCode(failure_ ? failure_ : std::current_exception(), stage));
            }
        }
        if (releaseFailure_) {
            try {
                std::rethrow_exception(releaseFailure_);
            } catch (const std::exception& error) {
                reportDesktopFailure("input_release", error.what(), session_);
                co_return protocol::desktopFailureExitCode(wire::DesktopFault::INPUT_RELEASE, desktopFailureCode(releaseFailure_, wire::DesktopFault::INPUT_RELEASE));
            }
        }
        co_return 0;
    }

  private:
    void rememberFailure(const char* stage) {
        if (!stopped_ && !failure_) {
            failure_ = std::current_exception();
            failureStage_ = stage;
        }
    }
    void reportFailure() const noexcept {
        if (!failure_) {
            return;
        }
        try {
            std::rethrow_exception(failure_);
        } catch (const std::exception& error) {
            reportDesktopFailure(failureStage_, error.what(), session_);
        } catch (...) {
            reportDesktopFailure(failureStage_, "Unknown desktop worker exception", session_);
        }
    }
    struct OutstandingProbe {
        std::uint32_t nonce;
        std::chrono::steady_clock::time_point sentAt;
    };

    NamedPipeTransport& pipe_;
    std::optional<wire::Envelope> secureAttention_;
    asio::steady_timer probeTimer_;
    DesktopVideoRuntime video_;
    protocol::DesktopPipeValidator incoming_, outgoing_;
    std::string session_;
    std::unique_ptr<DesktopInput> input_;
    std::exception_ptr releaseFailure_, failure_;
    const char* failureStage_ = "worker";
    bool writeControl_;
    DesktopOptimizationGate optimization_;
    bool pipelineEnabled_ = false;
    DesktopPipelineBuffer pipeline_;
    bool rebindRequested_ = false;
    std::optional<wire::DesktopResize> resize_;
    bool resizing_ = false;
    std::uint64_t sequence_ = 0;
    std::uint64_t inputGeneration_ = 0;
    std::uint32_t probeNonce_ = 0;
    std::uint32_t lastAcceptedResponseNonce_ = 0;
    std::deque<OutstandingProbe> outstandingProbes_;
    std::deque<std::uint32_t> confirmedProbes_;
    std::chrono::steady_clock::time_point lastAck_{}, nextProbe_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool stopped_ = false, ready_ = false, refresh_ = false, requestedClose_ = false;
};
} // namespace
asio::awaitable<std::uint32_t> runDesktopWorker(ruvia::execution_handle loop, NamedPipeTransport& pipe, std::uint32_t windowsSession) {
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::seconds(10));
    deadline.async_wait([&pipe](std::error_code error) { if (!error) { pipe.cancel(); } });
    bool running = false;
    try {
        auto first = co_await pipe.receive();
        deadline.cancel();
        auto start = protocol::decodeMessage(first);
        if (!start || !start->has_desktop_start()) {
            throw DesktopFailure(wire::DesktopFault::PROTOCOL_REJECTED, "Desktop IPC requires a startup message");
        }
        DesktopWorker worker(std::move(loop), pipe, windowsSession, *start);
        running = true;
        co_return co_await worker.run();
    } catch (const std::exception& error) {
        deadline.cancel();
        if (!running) {
            reportDesktopFailure("startup", error.what());
        }
        co_return protocol::desktopFailureExitCode(wire::DesktopFault::WORKER_STARTUP, desktopFailureCode(std::current_exception(), wire::DesktopFault::WORKER_STARTUP));
    }
}
} // namespace ultraterimal::agent
