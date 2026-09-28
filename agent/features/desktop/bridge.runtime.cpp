#include "bridge.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "desktop.transport.h"
#include "platform/windows/pipe.transport.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace ultraterimal::agent {
namespace {
class Bridge {
  public:
    Bridge(ruvia::EventLoop loop, DesktopTransport& remote, NamedPipeTransport& pipe,
           std::string session, std::uint32_t windowsSession, bool writeControl)
        : remote_(remote), pipe_(pipe), deadline_(loop.executor()), session_(std::move(session)),
          incoming_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kDesktop}),
          outgoing_({session_, windowsSession, writeControl, protocol::DesktopPipeSource::kService}) {}
    ~Bridge() {
        *alive_ = false;
        stop();
    }
    void stop() {
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
            co_await (upload() && download() && watch());
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
                if (terminal(message)) {
                    terminal_ = std::move(message);
                    stop();
                    co_return;
                }
                if (message.channel_id() == "video") {
                    co_await remote_.sendVideo(std::move(message));
                } else {
                    co_await remote_.sendCursor(std::move(message));
                }
            }
        } catch (...) {
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
            stop();
            throw;
        }
    }
    asio::awaitable<void> watch() {
        try {
            co_await remote_.watchVideo();
        } catch (...) {
            stop();
            throw;
        }
    }
    DesktopTransport& remote_;
    NamedPipeTransport& pipe_;
    asio::steady_timer deadline_;
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
