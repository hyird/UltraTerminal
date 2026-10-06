#include "desktop.transport.h"

#include <array>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "features/connection/connection.transport.h"
#include "optimization.runtime.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::agent {
struct DesktopTransport::State : std::enable_shared_from_this<State> {
    std::array<std::unique_ptr<DeviceTransport>, 2> sockets;
    std::array<std::unique_ptr<asio::steady_timer>, 2> writeDeadlines;
    std::array<std::string, 2> channelIds;
    std::array<std::string, 2> tickets;
    std::array<std::unique_ptr<protocol::DesktopChannelValidator>, 2> outgoing;
    std::unique_ptr<protocol::DesktopChannelValidator> incoming;
    std::array<std::uint64_t, 2> sequences{1, 1};
    std::array<std::uint32_t, 2> serverOptimizationContracts{0, 0};
    std::string sessionId;
    asio::steady_timer deadline;
    bool started = false;
    bool ready = false;
    bool cancelled = false;
    bool browserRequestedV2 = false;

    State(ruvia::execution_handle loop, ConnectionConfig config, const wire::Envelope& open) : deadline(loop.executor()) {
        if (!protocol::validateMessage(open) || !open.has_session_open() || open.session_open().kind() != wire::DESKTOP || !open.channel_id().empty()) {
            throw std::invalid_argument("Invalid desktop session grant");
        }
        sessionId = open.session_id();
        for (const auto& grant : open.session_open().channels()) {
            const std::size_t slot = grant.purpose() == wire::DESKTOP_INPUT ? 0 : 1;
            channelIds[slot] = grant.channel_id();
            tickets[slot] = grant.ticket();
            sockets[slot] = std::make_unique<DeviceTransport>(loop, config, protocol::kMaximumMessageBytes, wire::NativeConnect::DESKTOP_CHANNEL);
            writeDeadlines[slot] = std::make_unique<asio::steady_timer>(loop.executor());
            outgoing[slot] = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{sessionId, grant.channel_id(), grant.purpose(), protocol::ChannelSource::kAgent, open.session_open().write_control()});
        }
        incoming = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{sessionId, channelIds[0], wire::DESKTOP_INPUT, protocol::ChannelSource::kBrowser, open.session_open().write_control()});
    }
    std::uint32_t serverOptimizationContract() const {
        return ready ? DesktopOptimizationGate::serverContract(serverOptimizationContracts[0], serverOptimizationContracts[1]) : 0;
    }
    void cancel() {
        cancelled = true;
        ready = false;
        deadline.cancel();
        for (auto& socket : sockets) {
            socket->cancel();
        }
        for (auto& timer : writeDeadlines) {
            timer->cancel();
        }
        for (auto& ticket : tickets) {
            ticket.clear();
        }
    }
    asio::awaitable<void> openChannel(std::size_t slot, std::string credential) {
        try {
            co_await sockets[slot]->connect(std::move(credential));
            if (cancelled) {
                throw std::runtime_error("Desktop connection cancelled");
            }
            wire::Envelope open;
            open.set_protocol_version(protocol::kVersion);
            open.set_session_id(sessionId);
            open.set_channel_id(channelIds[slot]);
            open.set_sequence(1);
            const auto purpose = slot == 0 ? wire::DESKTOP_INPUT : wire::DESKTOP_VIDEO;
            open.mutable_channel_open()->set_purpose(purpose);
            open.mutable_channel_open()->set_ticket(std::move(tickets[slot]));
            auto encoded = protocol::encodeMessage(open);
            if (!encoded) {
                throw std::runtime_error("Cannot encode desktop channel grant");
            }
            open.Clear();
            co_await sockets[slot]->send(std::move(*encoded));
            auto bytes = co_await sockets[slot]->receive();
            if (bytes.size() > protocol::kMaximumControlBytes) {
                throw std::runtime_error("Oversized desktop readiness message");
            }
            const auto message = protocol::decodeMessage(bytes);
            if (!message || !message->has_channel_ready() || message->session_id() != sessionId || message->channel_id() != channelIds[slot] || message->channel_ready().purpose() != purpose) {
                throw std::runtime_error("Invalid desktop channel readiness");
            }
            serverOptimizationContracts[slot] = message->channel_ready().desktop_optimization_contract();
        } catch (...) {
            cancel();
            throw;
        }
    }
    static asio::awaitable<void> connectOwned(std::shared_ptr<State> self, std::string credential) {
        if (self->started || self->cancelled) {
            throw std::logic_error("Desktop transport can only connect once");
        }
        self->started = true;
        self->deadline.expires_after(std::chrono::seconds(60));
        self->deadline.async_wait([weak = self->weak_from_this()](std::error_code error) {
            if (auto state = weak.lock(); state && !error && !state->ready) {
                state->cancel();
            }
        });
        try {
            using namespace asio::experimental::awaitable_operators;
            co_await (self->openChannel(0, credential) && self->openChannel(1, credential));
            if (self->cancelled) {
                throw std::runtime_error("Desktop pairing cancelled");
            }
            self->ready = true;
            // Input cleanup ACKs remain independent of writable/frame binding.
            // Video starts with allow0 even when both Server gates are 2:
            // Config2 must advertise support without activating v2 early.
            const auto contract = self->serverOptimizationContract();
            self->outgoing[0]->setOptimizationContract(contract);
            self->outgoing[1]->setOptimizationContract(0);
            self->incoming->setOptimizationContract(contract);
            self->deadline.cancel();
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<std::chrono::microseconds> sendOwned(std::shared_ptr<State> self, std::size_t slot, wire::Envelope message, std::uint32_t allowedOptimizationContract = 0) {
        try {
            if (!self->ready || self->sequences[slot] == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("Desktop channel is not writable");
            }
            message.set_protocol_version(protocol::kVersion);
            message.set_session_id(self->sessionId);
            message.set_channel_id(self->channelIds[slot]);
            message.set_sequence(++self->sequences[slot]);
            if (slot == 1 && message.has_video_config()) {
                // This snapshot travelled with the queue entry. Do not replace
                // it with newer capabilities received while that entry waited.
                if (allowedOptimizationContract == 2 && !self->browserRequestedV2) {
                    throw std::runtime_error("Desktop v2 configuration preceded browser negotiation");
                }
                self->outgoing[1]->setOptimizationContract(self->serverOptimizationContract() == 2 ? allowedOptimizationContract : 0);
            }
            if (!self->outgoing[slot]->accept(message)) {
                throw std::runtime_error("Invalid outgoing desktop message");
            }
            auto encoded = protocol::encodeMessage(message);
            if (!encoded) {
                throw std::runtime_error("Cannot encode desktop message");
            }
            self->writeDeadlines[slot]->expires_after(std::chrono::seconds(10));
            self->writeDeadlines[slot]->async_wait([weak = self->weak_from_this(), slot, sequence = self->sequences[slot]](std::error_code error) {
                if (auto state = weak.lock(); state && !error && state->ready && state->sequences[slot] == sequence) {
                    state->cancel();
                }
            });
            const auto started = std::chrono::steady_clock::now();
            co_await self->sockets[slot]->send(std::move(*encoded));
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
            self->writeDeadlines[slot]->cancel();
            co_return elapsed;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<wire::Envelope> receiveOwned(std::shared_ptr<State> self) {
        try {
            if (!self->ready) {
                throw std::runtime_error("Desktop channel is not readable");
            }
            const auto bytes = co_await self->sockets[0]->receive();
            auto message = protocol::decodeMessage(bytes);
            if (!message || !self->incoming->accept(*message)) {
                throw std::runtime_error("Invalid incoming desktop input");
            }
            if (message->has_desktop_video_capabilities() && message->desktop_video_capabilities().desktop_optimization_contract() == 2) {
                self->browserRequestedV2 = true;
            }
            co_return std::move(*message);
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> watchOwned(std::shared_ptr<State> self) {
        try {
            if (!self->ready) {
                throw std::runtime_error("Desktop channel is not readable");
            }
            (void)co_await self->sockets[1]->receive();
            throw std::runtime_error("Unexpected incoming desktop video");
        } catch (...) {
            self->cancel();
            throw;
        }
    }
};
DesktopTransport::DesktopTransport(ruvia::execution_handle loop, ConnectionConfig config, wire::Envelope sessionOpen)
    : state_(std::make_shared<State>(std::move(loop), std::move(config), sessionOpen)) {}
DesktopTransport::~DesktopTransport() { state_->cancel(); }
asio::awaitable<void> DesktopTransport::connect(std::string credential) { return State::connectOwned(state_, std::move(credential)); }
asio::awaitable<std::chrono::microseconds> DesktopTransport::sendVideo(wire::Envelope message, std::uint32_t allowedOptimizationContract) { return State::sendOwned(state_, 1, std::move(message), allowedOptimizationContract); }
asio::awaitable<void> DesktopTransport::sendCursor(wire::Envelope message) { (void)co_await State::sendOwned(state_, 0, std::move(message)); }
asio::awaitable<wire::Envelope> DesktopTransport::receiveInput() { return State::receiveOwned(state_); }
asio::awaitable<void> DesktopTransport::watchVideo() { return State::watchOwned(state_); }
std::uint32_t DesktopTransport::serverOptimizationContract() const { return state_->serverOptimizationContract(); }
void DesktopTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
