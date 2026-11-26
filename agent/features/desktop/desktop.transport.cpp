#include "desktop.transport.h"

#include <array>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio_bridge.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <one_shot.h>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "features/connection/connection.transport.h"
#include "optimization.runtime.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::agent {
namespace {
template <typename T>
struct CrossLoopOutcome {
    using Value = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;
    [[no_unique_address]] Value value{};
    std::exception_ptr failure;
};
template <typename T>
struct CrossLoopGuard {
    ruvia::one_shot_completion<CrossLoopOutcome<T>> completion;
    explicit CrossLoopGuard(ruvia::one_shot_completion<CrossLoopOutcome<T>> target) noexcept : completion(std::move(target)) {}
    CrossLoopGuard(const CrossLoopGuard&) = delete;
    CrossLoopGuard& operator=(const CrossLoopGuard&) = delete;
    CrossLoopGuard(CrossLoopGuard&&) noexcept = default;
    CrossLoopGuard& operator=(CrossLoopGuard&&) = delete;
    ~CrossLoopGuard() {
        if (completion.valid()) {
            CrossLoopOutcome<T> aborted;
            aborted.failure = std::make_exception_ptr(std::runtime_error("Desktop transport loop stopped"));
            (void)completion.complete(std::move(aborted));
        }
    }
    void answer(CrossLoopOutcome<T> outcome) {
        const auto target = std::move(completion);
        (void)target.complete(std::move(outcome));
    }
};
template <typename T>
ruvia::task<CrossLoopOutcome<T>> waitCrossLoop(ruvia::one_shot_receiver<CrossLoopOutcome<T>>& receiver) {
    auto waited = co_await receiver.wait();
    if (!waited.has_value()) {
        CrossLoopOutcome<T> aborted;
        aborted.failure = std::make_exception_ptr(std::runtime_error("Desktop caller loop stopped"));
        co_return aborted;
    }
    co_return std::move(waited).take_value();
}
template <typename T, typename Fn>
asio::awaitable<T> onLoop(ruvia::event_loop caller, ruvia::event_loop target, Fn makeAwaitable) {
    if (target.worker().is_current()) {
        co_return co_await makeAwaitable();
    }
    auto [completion, receiver] = ruvia::make_one_shot<CrossLoopOutcome<T>>(caller.worker());
    const auto executor = target.executor();
    const auto posted = target.worker().post([executor, guard = CrossLoopGuard<T>(std::move(completion)), make = std::move(makeAwaitable)]() mutable {
        if constexpr (std::is_void_v<T>) {
            asio::co_spawn(executor, make(), [guard = std::move(guard)](std::exception_ptr error) mutable {
                CrossLoopOutcome<void> outcome;
                outcome.failure = std::move(error);
                guard.answer(std::move(outcome));
            });
        } else {
            asio::co_spawn(executor, make(), [guard = std::move(guard)](std::exception_ptr error, T result) mutable {
                CrossLoopOutcome<T> outcome;
                if (error) {
                    outcome.failure = std::move(error);
                } else {
                    outcome.value.emplace(std::move(result));
                }
                guard.answer(std::move(outcome));
            });
        }
    });
    if (!posted.accepted()) {
        throw std::runtime_error("Desktop transport loop unavailable");
    }
    auto outcome = co_await ruvia::as_awaitable(waitCrossLoop(receiver));
    if (outcome.failure) {
        std::rethrow_exception(outcome.failure);
    }
    if constexpr (!std::is_void_v<T>) {
        co_return std::move(*outcome.value);
    }
}
} // namespace
struct DesktopTransport::State : std::enable_shared_from_this<State> {
    ruvia::event_loop ownerLoop;
    ruvia::event_loop inputLoop;
    ruvia::event_loop videoLoop;
    std::array<std::unique_ptr<DeviceTransport>, 2> sockets;
    std::array<std::unique_ptr<asio::steady_timer>, 2> writeDeadlines;
    std::array<std::string, 2> channelIds;
    std::array<std::string, 2> tickets;
    std::array<std::unique_ptr<protocol::DesktopChannelValidator>, 2> outgoing;
    std::unique_ptr<protocol::DesktopChannelValidator> incoming;
    std::array<std::uint64_t, 2> sequences{1, 1};
    std::array<std::uint32_t, 2> serverOptimizationContracts{0, 0};
    std::array<bool, 2> serverContinuous{false, false};
    std::string sessionId;
    asio::steady_timer deadline;
    bool started = false;
    std::atomic_bool ready{false};
    std::atomic_bool cancelled{false};
    std::atomic_bool browserRequestedV2{false};
    std::atomic_bool browserRequestedContinuous{false};

    State(ruvia::event_loop owner, ConnectionConfig config, const wire::Envelope& open, ruvia::event_loop input, ruvia::event_loop video)
        : ownerLoop(std::move(owner)), inputLoop(input.valid() ? std::move(input) : ownerLoop), videoLoop(video.valid() ? std::move(video) : ownerLoop), deadline(ownerLoop.executor()) {
        if (!protocol::validateMessage(open) || !open.has_session_open() || open.session_open().kind() != wire::DESKTOP || !open.channel_id().empty()) {
            throw std::invalid_argument("Invalid desktop session grant");
        }
        sessionId = open.session_id();
        for (const auto& grant : open.session_open().channels()) {
            const std::size_t slot = grant.purpose() == wire::DESKTOP_INPUT ? 0 : 1;
            const auto& slotLoop = slot == 0 ? inputLoop : videoLoop;
            channelIds[slot] = grant.channel_id();
            tickets[slot] = grant.ticket();
            sockets[slot] = std::make_unique<DeviceTransport>(slotLoop, config, protocol::kMaximumMessageBytes, wire::NativeConnect::DESKTOP_CHANNEL);
            writeDeadlines[slot] = std::make_unique<asio::steady_timer>(slotLoop.executor());
            outgoing[slot] = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{sessionId, grant.channel_id(), grant.purpose(), protocol::ChannelSource::kAgent, open.session_open().write_control()});
        }
        incoming = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{sessionId, channelIds[0], wire::DESKTOP_INPUT, protocol::ChannelSource::kBrowser, open.session_open().write_control()});
    }
    std::uint32_t serverOptimizationContract() const {
        return ready.load(std::memory_order_acquire) ? DesktopOptimizationGate::serverContract(serverOptimizationContracts[0], serverOptimizationContracts[1]) : 0;
    }
    bool serverContinuousFrames() const {
        return serverOptimizationContract() == 2 && serverContinuous[0] && serverContinuous[1];
    }
    void cancelSlot(std::size_t slot) {
        if (sockets[slot]) {
            sockets[slot]->cancel();
        }
        if (writeDeadlines[slot]) {
            writeDeadlines[slot]->cancel();
        }
        tickets[slot].clear();
    }
    void cancel() {
        cancelled.store(true, std::memory_order_release);
        ready.store(false, std::memory_order_release);
        if (ownerLoop.valid() && ownerLoop.worker().is_current()) {
            deadline.cancel();
        } else if (ownerLoop.valid()) {
            (void)ownerLoop.defer_cleanup([self = shared_from_this()] { self->deadline.cancel(); });
        }
        const auto dispatchCancel = [this](const ruvia::event_loop& target, std::size_t slot) {
            if (!target.valid() || target.worker().is_current()) {
                cancelSlot(slot);
            } else {
                (void)target.defer_cleanup([self = shared_from_this(), slot] { self->cancelSlot(slot); });
            }
        };
        dispatchCancel(inputLoop, 0);
        dispatchCancel(videoLoop, 1);
    }
    asio::awaitable<void> openChannel(std::size_t slot, std::string credential) {
        try {
            co_await sockets[slot]->connect(std::move(credential));
            if (cancelled.load(std::memory_order_acquire)) {
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
            serverContinuous[slot] = message->channel_ready().continuous_frames();
        } catch (...) {
            cancel();
            throw;
        }
    }
    static asio::awaitable<void> connectOwned(std::shared_ptr<State> self, std::string credential) {
        if (self->started || self->cancelled.load(std::memory_order_acquire)) {
            throw std::logic_error("Desktop transport can only connect once");
        }
        self->started = true;
        self->deadline.expires_after(std::chrono::seconds(60));
        self->deadline.async_wait([weak = self->weak_from_this()](std::error_code error) {
            if (auto state = weak.lock(); state && !error && !state->ready.load(std::memory_order_acquire)) {
                state->cancel();
            }
        });
        try {
            using namespace asio::experimental::awaitable_operators;
            auto openInput = onLoop<void>(self->ownerLoop, self->inputLoop, [self, credential] {
                return self->openChannel(0, credential);
            });
            auto openVideo = onLoop<void>(self->ownerLoop, self->videoLoop, [self, credential] {
                return self->openChannel(1, credential);
            });
            co_await (std::move(openInput) && std::move(openVideo));
            if (self->cancelled.load(std::memory_order_acquire)) {
                throw std::runtime_error("Desktop pairing cancelled");
            }
            // Input cleanup ACKs remain independent of writable/frame binding.
            // Video starts with allow0 even when both Server gates are 2:
            // Config2 must advertise support without activating v2 early.
            const auto contract = DesktopOptimizationGate::serverContract(self->serverOptimizationContracts[0], self->serverOptimizationContracts[1]);
            self->outgoing[0]->setOptimizationContract(contract);
            self->outgoing[1]->setOptimizationContract(0);
            self->incoming->setOptimizationContract(contract);
            self->ready.store(true, std::memory_order_release);
            self->deadline.cancel();
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<std::chrono::microseconds> sendOwned(std::shared_ptr<State> self, std::size_t slot, wire::Envelope message, std::uint32_t allowedOptimizationContract = 0) {
        try {
            if (!self->ready.load(std::memory_order_acquire) || self->sequences[slot] == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("Desktop channel is not writable");
            }
            message.set_protocol_version(protocol::kVersion);
            message.set_session_id(self->sessionId);
            message.set_channel_id(self->channelIds[slot]);
            message.set_sequence(++self->sequences[slot]);
            if (slot == 1 && message.has_video_config()) {
                // This snapshot travelled with the queue entry. Do not replace
                // it with newer capabilities received while that entry waited.
                if (allowedOptimizationContract == 2 && !self->browserRequestedV2.load(std::memory_order_acquire)) {
                    throw std::runtime_error("Desktop v2 configuration preceded browser negotiation");
                }
                self->outgoing[1]->setOptimizationContract(self->serverOptimizationContract() == 2 ? allowedOptimizationContract : 0);
                self->outgoing[1]->setContinuousFrames(self->serverContinuousFrames() && self->browserRequestedContinuous.load(std::memory_order_acquire));
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
                if (auto state = weak.lock(); state && !error && state->ready.load(std::memory_order_acquire) && state->sequences[slot] == sequence) {
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
            if (!self->ready.load(std::memory_order_acquire)) {
                throw std::runtime_error("Desktop channel is not readable");
            }
            const auto bytes = co_await self->sockets[0]->receive();
            auto message = protocol::decodeMessage(bytes);
            if (!message || !self->incoming->accept(*message)) {
                throw std::runtime_error("Invalid incoming desktop input");
            }
            if (message->has_desktop_video_capabilities() && message->desktop_video_capabilities().desktop_optimization_contract() == 2) {
                if (message->desktop_video_capabilities().continuous_frames()) {
                    self->browserRequestedContinuous.store(true, std::memory_order_release);
                }
                self->browserRequestedV2.store(true, std::memory_order_release);
            }
            co_return std::move(*message);
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> watchOwned(std::shared_ptr<State> self) {
        try {
            if (!self->ready.load(std::memory_order_acquire)) {
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
DesktopTransport::DesktopTransport(ruvia::event_loop ownerLoop, ConnectionConfig config, wire::Envelope sessionOpen, ruvia::event_loop inputLoop, ruvia::event_loop videoLoop)
    : state_(std::make_shared<State>(std::move(ownerLoop), std::move(config), sessionOpen, std::move(inputLoop), std::move(videoLoop))) {}
DesktopTransport::~DesktopTransport() { state_->cancel(); }
asio::awaitable<void> DesktopTransport::connect(std::string credential) { return State::connectOwned(state_, std::move(credential)); }
asio::awaitable<std::chrono::microseconds> DesktopTransport::sendVideo(wire::Envelope message, std::uint32_t allowedOptimizationContract) {
    return onLoop<std::chrono::microseconds>(state_->ownerLoop, state_->videoLoop, [state = state_, message = std::move(message), allowedOptimizationContract]() mutable {
        return State::sendOwned(state, 1, std::move(message), allowedOptimizationContract);
    });
}
asio::awaitable<void> DesktopTransport::sendCursor(wire::Envelope message) {
    (void)co_await onLoop<std::chrono::microseconds>(state_->ownerLoop, state_->inputLoop, [state = state_, message = std::move(message)]() mutable {
        return State::sendOwned(state, 0, std::move(message));
    });
}
asio::awaitable<wire::Envelope> DesktopTransport::receiveInput() {
    return onLoop<wire::Envelope>(state_->ownerLoop, state_->inputLoop, [state = state_] { return State::receiveOwned(state); });
}
asio::awaitable<void> DesktopTransport::watchVideo() {
    return onLoop<void>(state_->ownerLoop, state_->videoLoop, [state = state_] { return State::watchOwned(state); });
}
std::uint32_t DesktopTransport::serverOptimizationContract() const { return state_->serverOptimizationContract(); }
bool DesktopTransport::serverContinuousFrames() const { return state_->serverContinuousFrames(); }
void DesktopTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
