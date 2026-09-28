#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Channel.h>

#include "ultraterimal/protocol/native.protocol.h"

namespace ultraterimal::server {
enum class NativeCloseCause { kUnknown,
                              kAuthorizationTimeout,
                              kReadTimeout,
                              kWriteTimeout,
                              kReadFailed,
                              kWriteFailed,
                              kHandshakeFailed,
                              kRuntimeFailed,
                              kServerAbort };
inline std::string_view nativeCloseCauseName(NativeCloseCause cause) {
    switch (cause) {
    case NativeCloseCause::kUnknown:
        return "unknown";
    case NativeCloseCause::kAuthorizationTimeout:
        return "authorization_timeout";
    case NativeCloseCause::kReadTimeout:
        return "read_timeout";
    case NativeCloseCause::kWriteTimeout:
        return "write_timeout";
    case NativeCloseCause::kReadFailed:
        return "read_failed";
    case NativeCloseCause::kWriteFailed:
        return "write_failed";
    case NativeCloseCause::kHandshakeFailed:
        return "handshake_failed";
    case NativeCloseCause::kRuntimeFailed:
        return "runtime_failed";
    case NativeCloseCause::kServerAbort:
        return "server_abort";
    }
    return "unknown";
}
struct NativePayload {
    std::string bytes;
    std::shared_ptr<std::atomic<std::size_t>> budget;
    NativePayload(std::size_t size, std::shared_ptr<std::atomic<std::size_t>> allocationBudget) : budget(std::move(allocationBudget)) {
        const auto previous = budget->fetch_add(size);
        if (previous + size > 256 * 1024 * 1024) {
            budget->fetch_sub(size);
            throw std::runtime_error("Native receive budget exceeded");
        }
        try {
            bytes.resize(size);
        } catch (...) {
            budget->fetch_sub(size);
            throw;
        }
    }
    ~NativePayload() { budget->fetch_sub(bytes.size()); }
    NativePayload(const NativePayload&) = delete;
    NativePayload& operator=(const NativePayload&) = delete;
};
class NativeMessage {
  public:
    explicit NativeMessage(std::shared_ptr<NativePayload> payload) : payload_(std::move(payload)) {}
    bool binary() const { return true; }
    std::string_view payload() const { return payload_->bytes; }

  private:
    std::shared_ptr<NativePayload> payload_;
};
// The bounded channels are the only interface between the native Ruvia Worker
// and the Web/business Worker. Protocol handlers never touch native sockets.
class NativeChannel {
  public:
    explicit NativeChannel(ruvia::WorkerHandle networkWorker)
        : outgoing(ruvia::makeChannel<std::string>(networkWorker, {.capacity = 1})),
          readCredit(ruvia::makeChannel<bool>(networkWorker, {.capacity = 1})),
          initialized(ruvia::makeChannel<bool>(networkWorker, {.capacity = 1})) {}
    void attach(ruvia::WorkerHandle businessWorker) {
        incoming.emplace(ruvia::makeChannel<NativeMessage>(businessWorker, {.capacity = 1}));
        writeDone.emplace(ruvia::makeChannel<bool>(businessWorker, {.capacity = 1}));
        attached.store(true);
        if (!initialized.first.send(true).accepted()) {
            incoming->first.close();
            writeDone->first.close();
            throw std::runtime_error("Native channel setup cancelled");
        }
    }
    ruvia::Task<std::optional<NativeMessage>> read() {
        auto result = co_await incoming->second.receive();
        if (!result.hasValue()) {
            co_return std::nullopt;
        }
        if (!readCredit.first.send(true).accepted()) {
            throw std::runtime_error("Native read credit rejected");
        }
        co_return std::move(result).takeValue();
    }
    ruvia::Task<void> binary(std::string_view bytes) {
        if (bytes.empty() || bytes.size() > protocol::kMaximumMessageBytes || !outgoing.first.send(std::string(bytes)).accepted()) {
            throw std::runtime_error("Native write rejected");
        }
        auto result = co_await writeDone->second.receive();
        if (!result.hasValue() || !result.value()) {
            throw std::runtime_error("Native write failed");
        }
    }
    void abort() { abortNetwork(); }
    void recordCloseCause(NativeCloseCause cause) {
        auto unknown = NativeCloseCause::kUnknown;
        (void)closeCause.compare_exchange_strong(unknown, cause);
    }
    void closeQueues() {
        outgoing.first.close();
        readCredit.first.close();
        initialized.first.close();
        if (attached.load()) {
            incoming->first.close();
            writeDone->first.close();
        }
    }
    std::function<void()> abortNetwork;
    std::atomic<NativeCloseCause> closeCause = NativeCloseCause::kUnknown;
    std::atomic<bool> attached = false;
    std::function<void(std::size_t)> authorizeNetwork;
    std::pair<ruvia::ChannelSender<std::string>, ruvia::ChannelReceiver<std::string>> outgoing;
    std::pair<ruvia::ChannelSender<bool>, ruvia::ChannelReceiver<bool>> readCredit, initialized;
    std::optional<std::pair<ruvia::ChannelSender<NativeMessage>, ruvia::ChannelReceiver<NativeMessage>>> incoming;
    std::optional<std::pair<ruvia::ChannelSender<bool>, ruvia::ChannelReceiver<bool>>> writeDone;
};
} // namespace ultraterimal::server
