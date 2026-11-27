#include "features/terminal/terminal.runtime.h"

#include <algorithm>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "features/connection/connection.transport.h"
#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
constexpr std::size_t kTerminalChunkBytes = 32 * 1024;
} // namespace

struct TerminalSession::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    ConnectionConfig config;
    wire::Envelope sessionOpen;
    std::string sessionId;
    std::string channelId;
    std::string ticket;
    std::unique_ptr<DeviceTransport> transport;
    std::unique_ptr<PlatformTerminal> terminal;
    std::uint64_t outgoingSequence = 1;
    std::uint64_t incomingSequence = 1;
    std::uint32_t exitCode = 0;
    bool stopped = false;
    bool eofSent = false;

    State(ruvia::event_loop owner, ConnectionConfig settings, wire::Envelope grant)
        : loop(std::move(owner)), config(std::move(settings)), sessionOpen(std::move(grant)), sessionId(sessionOpen.session_id()) {
        if (sessionOpen.session_open().kind() != wire::SSH || sessionOpen.session_open().channels_size() != 1 ||
            !sessionOpen.session_open().write_control()) {
            throw std::invalid_argument("Invalid terminal session grant");
        }
        const auto& channel = sessionOpen.session_open().channels(0);
        if (channel.purpose() != wire::SSH_STREAM || channel.channel_id().empty() || channel.ticket().empty()) {
            throw std::invalid_argument("Invalid terminal channel grant");
        }
        channelId = channel.channel_id();
        ticket = channel.ticket();
    }

    void stop() {
        stopped = true;
        if (terminal) {
            terminal->close();
        }
        if (transport) {
            transport->cancel();
        }
    }

    std::string encodeEnvelope(wire::Envelope message) {
        if (outgoingSequence == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Terminal sequence exhausted");
        }
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id(sessionId);
        message.set_channel_id(channelId);
        message.set_sequence(++outgoingSequence);
        auto bytes = protocol::encodeMessage(message);
        if (!bytes) {
            throw std::runtime_error("Cannot encode terminal message");
        }
        return std::move(*bytes);
    }

    asio::awaitable<void> writeLoop() {
        try {
            while (!stopped) {
                auto data = co_await terminal->read();
                if (data.empty()) {
                    exitCode = terminal->waitExit();
                    wire::Envelope exitMessage;
                    exitMessage.mutable_terminal_exit()->set_exit_code(exitCode);
                    co_await transport->send(encodeEnvelope(std::move(exitMessage)));
                    eofSent = true;
                    break;
                }
                std::size_t offset = 0;
                while (offset < data.size() && !stopped) {
                    const std::size_t take = std::min(kTerminalChunkBytes, data.size() - offset);
                    wire::Envelope chunk;
                    chunk.mutable_terminal_data()->set_data(data.data() + offset, take);
                    co_await transport->send(encodeEnvelope(std::move(chunk)));
                    offset += take;
                }
            }
        } catch (...) {
            if (!stopped) {
                stop();
                throw;
            }
        }
        stop();
    }

    asio::awaitable<void> readLoop() {
        try {
            while (!stopped) {
                const auto raw = co_await transport->receive();
                const auto message = protocol::decodeMessage(raw);
                if (!message || message->session_id() != sessionId || message->channel_id() != channelId ||
                    message->sequence() <= incomingSequence) {
                    throw std::runtime_error("Invalid terminal message");
                }
                incomingSequence = message->sequence();
                if (message->has_terminal_data()) {
                    co_await terminal->write(message->terminal_data().data());
                } else if (message->has_terminal_resize()) {
                    terminal->resize(message->terminal_resize().columns(), message->terminal_resize().rows());
                } else if (message->has_session_close() || message->has_session_error()) {
                    break;
                } else {
                    throw std::runtime_error("Unexpected terminal command");
                }
            }
        } catch (...) {
            if (!stopped) {
                stop();
                throw;
            }
        }
        stop();
    }

    static asio::awaitable<TerminalSessionResult> run(std::shared_ptr<State> self, std::string credential) {
        TerminalSessionResult result;
        result.terminal.set_session_id(self->sessionId);
        try {
            self->transport = std::make_unique<DeviceTransport>(self->loop, self->config, protocol::kMaximumMessageBytes, wire::NativeConnect::TERMINAL_CHANNEL);
            asio::steady_timer connectDeadline(self->loop.executor());
            connectDeadline.expires_after(std::chrono::seconds(15));
            connectDeadline.async_wait([self](const std::error_code& error) {
                if (!error) {
                    self->stop();
                }
            });
            co_await self->transport->connect(std::move(credential));
            wire::Envelope open;
            open.set_protocol_version(protocol::kVersion);
            open.set_sequence(1);
            open.set_session_id(self->sessionId);
            open.set_channel_id(self->channelId);
            open.mutable_channel_open()->set_purpose(wire::SSH_STREAM);
            open.mutable_channel_open()->set_ticket(self->ticket);
            auto openBytes = protocol::encodeMessage(open);
            self->ticket.clear();
            if (!openBytes) {
                throw std::runtime_error("Cannot encode terminal channel open");
            }
            co_await self->transport->send(std::move(*openBytes));
            const auto readyRaw = co_await self->transport->receive();
            const auto ready = protocol::decodeMessage(readyRaw);
            if (!ready || !ready->has_channel_ready() || ready->session_id() != self->sessionId ||
                ready->channel_id() != self->channelId || ready->channel_ready().purpose() != wire::SSH_STREAM) {
                throw std::runtime_error("Terminal channel readiness rejected");
            }
            connectDeadline.cancel();
            if (self->stopped) {
                result.terminal.mutable_session_close()->set_reason("cancelled");
                co_return result;
            }
            self->terminal = PlatformTerminal::spawn(self->loop);
            using namespace asio::experimental::awaitable_operators;
            co_await (self->writeLoop() && self->readLoop());
            result.exitCode = self->exitCode;
            result.terminal.mutable_session_close()->set_reason(self->eofSent ? "terminal_exited" : "closed");
        } catch (const std::exception&) {
            self->stop();
            result.terminal.Clear();
            result.terminal.set_session_id(self->sessionId);
            result.terminal.mutable_session_error()->set_code("terminal_failed");
            result.terminal.mutable_session_error()->set_message("Terminal session failed");
        }
        self->stop();
        if (self->transport) {
            self->transport.reset();
        }
        if (self->terminal) {
            self->terminal.reset();
        }
        co_return result;
    }
};

TerminalSession::TerminalSession(ruvia::event_loop loop, ConnectionConfig config, wire::Envelope sessionOpen)
    : state_(std::make_shared<State>(std::move(loop), std::move(config), std::move(sessionOpen))) {}

TerminalSession::~TerminalSession() { stop(); }

asio::awaitable<TerminalSessionResult> TerminalSession::run(std::string credential) {
    return State::run(state_, std::move(credential));
}

void TerminalSession::stop() {
    if (state_) {
        state_->stop();
    }
}
} // namespace ultraterimal::agent
