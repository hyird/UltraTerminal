#include "terminal.runtime.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace ultraterimal::server {
TerminalDelivery::~TerminalDelivery() {
    if (budget_) {
        budget_->fetch_sub(bytes_.size(), std::memory_order_acq_rel);
    }
}

TerminalEndpoint::TerminalEndpoint(ruvia::worker_ref ioWorker, protocol::ChannelSource source)
    : source_(source), queue_(ruvia::make_channel<std::unique_ptr<TerminalDelivery>>(std::move(ioWorker), {.capacity_ = 64})) {}

ruvia::task<std::unique_ptr<TerminalDelivery>> TerminalEndpoint::receive(ruvia::stop_token stop) {
    auto result = co_await queue_.second.receive({.stop_token_ = std::move(stop)});
    if (!result.has_value()) {
        co_return nullptr;
    }
    auto delivery = std::move(result).take_value();
    if (closed_.load(std::memory_order_acquire)) {
        delivery.reset();
        while ((co_await queue_.second.receive()).has_value()) {
        }
        co_return nullptr;
    }
    co_return delivery;
}

void TerminalEndpoint::close() {
    closed_.store(true, std::memory_order_release);
    queue_.first.close();
}

TerminalSession::TerminalSession(ruvia::worker_ref worker) : worker_(std::move(worker)) {}

TerminalSession::~TerminalSession() { close(); }

std::shared_ptr<TerminalEndpoint> TerminalSession::attach(const ChannelAuthorization& authorization, protocol::ChannelSource source, ruvia::worker_ref ioWorker) {
    if (!worker_.is_current()) {
        throw std::logic_error("Terminal session accessed outside its Worker");
    }
    if (closed_.load(std::memory_order_acquire) || authorization.kind != wire::SSH || authorization.purpose != wire::SSH_STREAM ||
        !authorization.writeControl || authorization.sessionId.empty() || authorization.channelId.empty() ||
        (source != protocol::ChannelSource::kBrowser && source != protocol::ChannelSource::kAgent)) {
        return {};
    }
    if (bound_ && (identity_.sessionId != authorization.sessionId || identity_.channelId != authorization.channelId ||
                   identity_.userId != authorization.userId || identity_.deviceId != authorization.deviceId)) {
        return {};
    }
    const std::size_t slot = source == protocol::ChannelSource::kAgent ? 1 : 0;
    if (endpoints_[slot]) {
        return {};
    }
    auto endpointWorker = ioWorker.valid() ? std::move(ioWorker) : worker_;
    auto endpoint = std::shared_ptr<TerminalEndpoint>(new TerminalEndpoint(std::move(endpointWorker), source));
    if (!bound_) {
        identity_ = authorization;
        bound_ = true;
    }
    endpoints_[slot] = endpoint;
    if (endpoints_[0] && endpoints_[1]) {
        for (auto& item : endpoints_) {
            if (!item->queue_.first.send(std::unique_ptr<TerminalDelivery>(new TerminalDelivery())).accepted()) {
                close(TerminalCloseReason::kReadyQueueClosed);
                return {};
            }
        }
        ready_.store(true, std::memory_order_release);
        spdlog::info("terminal_session_ready session_id={} device_id={}", identity_.sessionId, identity_.deviceId);
    }
    return endpoint;
}

ruvia::task<bool> TerminalSession::forward(std::shared_ptr<TerminalEndpoint> sender, wire::Envelope message, ruvia::stop_token stop) {
    if (!worker_.is_current()) {
        throw std::logic_error("Terminal session accessed outside its Worker");
    }
    if (!sender || !ready() || message.session_id() != identity_.sessionId || message.channel_id() != identity_.channelId) {
        close(TerminalCloseReason::kInvalidBinding);
        co_return false;
    }
    const std::size_t sourceSlot = sender->source_ == protocol::ChannelSource::kAgent ? 1 : 0;
    const std::size_t targetSlot = 1 - sourceSlot;
    if (endpoints_[sourceSlot] != sender || !endpoints_[targetSlot]) {
        close(TerminalCloseReason::kInvalidBinding);
        co_return false;
    }
    if (message.sequence() <= incomingSequences_[sourceSlot]) {
        close(TerminalCloseReason::kMalformedMessage);
        co_return false;
    }
    incomingSequences_[sourceSlot] = message.sequence();

    const bool validDirection = sourceSlot == 0
                                    ? (message.has_terminal_data() || message.has_terminal_resize())
                                    : (message.has_terminal_data() || message.has_terminal_exit());
    if (!validDirection) {
        close(TerminalCloseReason::kInvalidDirection);
        co_return false;
    }

    message.set_protocol_version(protocol::kVersion);
    message.set_sequence(++outgoingSequences_[targetSlot]);
    auto encoded = protocol::encodeMessage(message);
    if (!encoded) {
        close(TerminalCloseReason::kMalformedMessage);
        co_return false;
    }

    auto delivery = std::unique_ptr<TerminalDelivery>(new TerminalDelivery());
    const std::size_t size = encoded->size();
    std::size_t used = budget_->load(std::memory_order_relaxed);
    while (size <= kMaximumSessionBytes && used <= kMaximumSessionBytes - size) {
        if (budget_->compare_exchange_weak(used, used + size, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            delivery->budget_ = budget_;
            delivery->bytes_ = std::move(*encoded);
            break;
        }
    }
    if (!delivery->budget_) {
        close(TerminalCloseReason::kBudgetExceeded);
        co_return false;
    }

    const auto sent = co_await endpoints_[targetSlot]->queue_.first.send_wait(std::move(delivery), {.timeout_ = std::chrono::seconds(10), .stop_token_ = std::move(stop)});
    if (!sent.accepted()) {
        close(TerminalCloseReason::kQueueClosed);
        co_return false;
    }
    co_return true;
}

void TerminalSession::close(TerminalCloseReason reason) {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    closeReason_ = reason;
    for (auto& endpoint : endpoints_) {
        if (endpoint) {
            endpoint->close();
        }
    }
}
} // namespace ultraterimal::server
