#include "bridge.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/BlockingPool.h>

#include "desktop.transport.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/pipe.transport.h"
#include "send_queue.runtime.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace ultraterimal::agent {
namespace {
class Bridge {
  public:
    Bridge(ruvia::EventLoop loop, DesktopTransport& remote, NamedPipeTransport& pipe,
           std::string session, std::uint32_t windowsSession, bool writeControl)
        : loop_(loop), windowsSession_(windowsSession), remote_(remote), pipe_(pipe), deadline_(loop.executor()), control_(loop.handle(), 32, 2 * 1024 * 1024), video_(loop.handle(), 32, 32 * 1024 * 1024), session_(std::move(session)),
          incoming_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kDesktop}),
          outgoing_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kService}) {}
    ~Bridge() {
        *alive_ = false;
        stop();
    }
    void stop() {
        stopped_ = true;
        deadline_.cancel();
        control_.close();
        video_.close();
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
        co_await pipe_.send(std::move(*bytes));
    }
    asio::awaitable<wire::Envelope> receiveLocal() {
        auto bytes = co_await pipe_.receive();
        auto message = protocol::decodeMessage(bytes);
        if (!message || !incoming_.accept(*message)) {
            throw std::runtime_error("Invalid desktop process output");
        }
        co_return std::move(*message);
    }
    asio::awaitable<wire::Envelope> run(std::uint32_t windowsSession, std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract) {
        deadline_.expires_after(std::chrono::seconds(10));
        deadline_.async_wait([this, alive = alive_](std::error_code error) { if (!error && *alive) { stop(); } });
        wire::Envelope start;
        start.mutable_desktop_start()->set_windows_session_id(windowsSession);
        start.mutable_desktop_start()->set_display_index(displayIndex);
        start.mutable_desktop_start()->set_write_control(writeControl);
        start.mutable_desktop_start()->set_desktop_input_contract(desktopInputContract);
        co_await sendLocal(std::move(start));
        auto first = co_await receiveLocal();
        deadline_.cancel();
        if (!first.has_desktop_ready()) {
            co_return first;
        }
        using namespace asio::experimental::awaitable_operators;
        try {
            co_await (upload() && write(control_, false) && write(video_, true) && download() && watch());
        } catch (...) {
            if (!terminal_) {
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
                        error = co_await ruvia::asAwaitable(ruvia::runBlocking(systemPool_, loop_.handle(), [session = windowsSession_] {
                            try {
                                return DesktopIdentity(session).sendSecureAttention();
                            } catch (const std::system_error& failure) {
                                return static_cast<std::uint32_t>(failure.code().value());
                            } catch (...) {
                                return std::uint32_t{5}; // ERROR_ACCESS_DENIED
                            }
                        }));
                    }
                    wire::Envelope result;
                    result.mutable_desktop_secure_attention_result()->set_win32_error(error);
                    if (!(co_await ruvia::asAwaitable(control_.send(std::move(result))))) {
                        throw std::runtime_error("Desktop result queue closed");
                    }
                    continue;
                }
                if (terminal(message)) {
                    terminal_ = std::move(message);
                    stop();
                    co_return;
                }
                auto& queue = message.channel_id() == "video" ? video_ : control_;
                if (!(co_await ruvia::asAwaitable(queue.send(std::move(message))))) {
                    throw std::runtime_error("Desktop send queue closed or message exceeds budget");
                }
            }
        } catch (...) {
            recordFailure("bridge_worker_read");
            stop();
            throw;
        }
    }
    asio::awaitable<void> write(DesktopSendQueue& queue, bool video) {
        try {
            while (auto message = co_await ruvia::asAwaitable(queue.receive())) {
                const auto bytes = message->ByteSizeLong();
                try {
                    if (video) {
                        co_await remote_.sendVideo(std::move(*message));
                    } else {
                        co_await remote_.sendCursor(std::move(*message));
                    }
                } catch (...) {
                    queue.complete(bytes);
                    throw;
                }
                queue.complete(bytes);
            }
        } catch (...) {
            recordFailure(video ? "bridge_video_send" : "bridge_control_send");
            stop();
            throw;
        }
    }
    asio::awaitable<void> download() {
        try {
            for (;;) {
                auto message = co_await remote_.receiveInput();
                co_await sendLocal(std::move(message));
            }
        } catch (...) {
            recordFailure("bridge_input_receive");
            stop();
            throw;
        }
    }
    asio::awaitable<void> watch() {
        try {
            co_await remote_.watchVideo();
        } catch (...) {
            recordFailure("bridge_video_receive");
            stop();
            throw;
        }
    }
    void recordFailure(const char* stage) const noexcept {
        if (stopped_) {
            return;
        }
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& error) {
            reportDesktopFailure(stage, error.what(), session_);
        } catch (...) {
            reportDesktopFailure(stage, "Unknown desktop bridge exception", session_);
        }
    }
    bool stopped_ = false;
    ruvia::EventLoop loop_;
    std::uint32_t windowsSession_;
    ruvia::BlockingPool systemPool_{{.threadCount = 1, .queueCapacity = 1}};
    std::chrono::steady_clock::time_point nextSecureAttention_{};
    DesktopTransport& remote_;
    NamedPipeTransport& pipe_;
    asio::steady_timer deadline_;
    DesktopSendQueue control_, video_;
    std::string session_;
    protocol::DesktopPipeValidator incoming_, outgoing_;
    std::uint64_t sequence_ = 0;
    std::optional<wire::Envelope> terminal_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
} // namespace
asio::awaitable<wire::Envelope> bridgeDesktop(ruvia::EventLoop loop, DesktopTransport& remote,
                                              NamedPipeTransport& pipe, std::string sessionId, std::uint32_t windowsSession,
                                              std::uint32_t displayIndex, bool writeControl, std::uint32_t desktopInputContract) {
    Bridge bridge(std::move(loop), remote, pipe, std::move(sessionId), windowsSession, writeControl);
    co_return co_await bridge.run(windowsSession, displayIndex, writeControl, desktopInputContract);
}
} // namespace ultraterimal::agent
