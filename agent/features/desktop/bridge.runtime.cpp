#include "bridge.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <event_loop.h>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "optimization.runtime.h"
#include "platform/windows/desktop_failure.error.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/pipe.transport.h"
#include "relay.runtime.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace ultraterimal::agent {
namespace {
class Bridge {
  public:
    Bridge(ruvia::event_loop loop, DesktopWorkerChannel& remote, NamedPipeTransport& pipe,
           std::string session, std::uint32_t windowsSession, bool writeControl)
        : loop_(loop), windowsSession_(windowsSession), remote_(remote), pipe_(pipe), deadline_(loop.executor()), session_(std::move(session)),
          incoming_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kDesktop}),
          outgoing_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kService}),
          optimization_(remote_.serverOptimizationContract()) {}
    ~Bridge() {
        *alive_ = false;
        stop();
    }
    void stop() {
        stopped_ = true;
        deadline_.cancel();
        pipe_.cancel();
        remote_.cancel();
    }
    asio::awaitable<void> sendLocal(wire::Envelope message) {
        if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop bridge sequence exhausted");
        }
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id(session_);
        message.set_sequence(++sequence_);
        message.set_channel_id(message.has_desktop_start() || terminal(message) ? "" : "input");
        if (!outgoing_.accept(message)) {
            throw std::runtime_error("Invalid service desktop command");
        }
        auto bytes = protocol::encodeMessage(message);
        if (!bytes) {
            throw std::runtime_error("Cannot encode service desktop command");
        }
        if (message.has_desktop_video_capabilities()) {
            optimization_.request(message.desktop_video_capabilities().desktop_optimization_contract());
            incoming_.setOptimizationContract(optimization_.allowedContract());
            outgoing_.setOptimizationContract(optimization_.allowedContract());
            incoming_.setContinuousFrames(remote_.serverContinuousFrames() && message.desktop_video_capabilities().continuous_frames());
        }
        co_await pipe_.send(std::move(*bytes));
    }
    asio::awaitable<wire::Envelope> receiveLocal() {
        auto bytes = co_await pipe_.receive();
        auto message = protocol::decodeMessage(bytes);
        if (!message || !incoming_.accept(*message)) {
            throw std::runtime_error("Invalid desktop process output");
        }
        if (message->has_video_config()) {
            optimization_.configure(message->video_config());
        }
        co_return std::move(*message);
    }
    asio::awaitable<wire::Envelope> run(std::uint32_t windowsSession, std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract) {
        deadline_.expires_after(std::chrono::seconds(10));
        deadline_.async_wait([this, alive = alive_](std::error_code error) { if (!error && *alive && waitingForReady_) { startupTimedOut_ = true; stop(); } });
        wire::Envelope start;
        start.mutable_desktop_start()->set_windows_session_id(windowsSession);
        start.mutable_desktop_start()->set_display_index(displayIndex);
        start.mutable_desktop_start()->set_write_control(writeControl);
        start.mutable_desktop_start()->set_desktop_input_contract(desktopInputContract);
        start.mutable_desktop_start()->set_desktop_optimization_contract(remote_.serverOptimizationContract());
        start.mutable_desktop_start()->set_continuous_frames(remote_.serverContinuousFrames());
        start.mutable_desktop_start()->set_desktop_audio(remote_.serverDesktopAudio());
        start.mutable_desktop_start()->set_audio_enabled(remote_.serverDesktopAudio() && remote_.audioEnabled());
        wire::Envelope first;
        try {
            co_await sendLocal(std::move(start));
            first = co_await receiveLocal();
            waitingForReady_ = false;
            deadline_.cancel();
        } catch (...) {
            waitingForReady_ = false;
            deadline_.cancel();
            if (startupTimedOut_) {
                throw DesktopFailure(wire::DesktopFault::STARTUP_TIMEOUT, "Desktop worker did not produce its first encoded frame before the startup deadline", wire::DesktopFault::WORKER_READY_WAIT);
            }
            throw;
        }
        if (!first.has_desktop_ready()) {
            co_return first;
        }
        using namespace asio::experimental::awaitable_operators;
        try {
            co_await (upload() && download());
        } catch (...) {
            if (!terminal_) {
                if (failure_) {
                    throw DesktopFailure(failure_->second, "Desktop bridge operation failed", failure_->first);
                }
                throw;
            }
        }
        co_return std::move(*terminal_);
    }

  private:
    static bool terminal(const wire::Envelope& message) {
        return message.has_session_close() || message.has_session_error();
    }
    asio::awaitable<void> upload() {
        try {
            for (;;) {
                auto message = co_await receiveLocal();
                if (message.has_input_event() && message.input_event().kind() == wire::InputEvent::SECURE_ATTENTION) {
                    const auto now = std::chrono::steady_clock::now();
                    std::uint32_t error = 1237; // ERROR_RETRY
                    if (now >= nextSecureAttention_) {
                        nextSecureAttention_ = now + std::chrono::seconds(2);
                        error = ((co_await ruvia::as_awaitable(ruvia::run_blocking(loop_.worker(), [session = windowsSession_] {
                                     try {
                                         return DesktopIdentity(session).sendSecureAttention();
                                     } catch (const std::system_error& failure) {
                                         return static_cast<std::uint32_t>(failure.code().value());
                                     } catch (...) {
                                         return std::uint32_t{5}; // ERROR_ACCESS_DENIED
                                     }
                                 }))).take_value());
                    }
                    wire::Envelope result;
                    result.mutable_desktop_secure_attention_result()->set_win32_error(error);
                    co_await remote_.sendCursor(std::move(result));
                    continue;
                }
                if (terminal(message)) {
                    terminal_ = std::move(message);
                    stop();
                    co_return;
                }
                if (message.channel_id() == "video") {
                    co_await remote_.sendVideo(std::move(message), optimization_.activeContract());
                } else if (message.has_audio_config() || message.has_audio_frame()) {
                    co_await remote_.sendAudio(std::move(message));
                } else {
                    co_await remote_.sendCursor(std::move(message));
                }
            }
        } catch (...) {
            recordFailure("bridge_worker_read");
            stop();
            throw;
        }
    }
    asio::awaitable<void> download() {
        try {
            for (;;) {
                auto entry = co_await remote_.receiveInput();
                try {
                    co_await sendLocal(std::move(entry.message));
                } catch (...) {
                    remote_.completeInput(entry);
                    throw;
                }
                remote_.completeInput(entry);
            }
        } catch (...) {
            recordFailure("control_ipc_write");
            stop();
            throw;
        }
    }
    void recordFailure(const char* stage) noexcept {
        if (stopped_ || remote_.cancelled() || failure_) {
            return;
        }
        const auto point = desktopFailureStage(stage);
        failure_ = std::pair{point, desktopFailureCode(std::current_exception(), point)};
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& error) {
            reportDesktopFailure(stage, error.what(), session_);
        } catch (...) {
            reportDesktopFailure(stage, "Unknown desktop bridge exception", session_);
        }
    }
    bool stopped_ = false, startupTimedOut_ = false, waitingForReady_ = true;
    std::optional<std::pair<wire::DesktopFault::Stage, wire::DesktopFault::Code>> failure_;
    ruvia::event_loop loop_;
    std::uint32_t windowsSession_;
    std::chrono::steady_clock::time_point nextSecureAttention_{};
    DesktopWorkerChannel& remote_;
    NamedPipeTransport& pipe_;
    asio::steady_timer deadline_;
    std::string session_;
    protocol::DesktopPipeValidator incoming_, outgoing_;
    DesktopOptimizationGate optimization_;
    std::uint64_t sequence_ = 0;
    std::optional<wire::Envelope> terminal_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace
asio::awaitable<wire::Envelope> bridgeDesktop(ruvia::event_loop loop, DesktopWorkerChannel& remote,
                                              NamedPipeTransport& pipe, std::string sessionId, std::uint32_t windowsSession,
                                              std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract) {
    Bridge bridge(std::move(loop), remote, pipe, std::move(sessionId), windowsSession, writeControl);
    co_return co_await bridge.run(windowsSession, displayIndex, writeControl, desktopInputContract);
}
} // namespace ultraterimal::agent
