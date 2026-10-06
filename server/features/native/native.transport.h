#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/channel.h>

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
    explicit NativeChannel(ruvia::execution_handle networkWorker)
        : outgoing(ruvia::make_channel<std::string>(networkWorker, {.capacity_ = 1})),
          readCredit(ruvia::make_channel<bool>(networkWorker, {.capacity_ = 1})),
          initialized(ruvia::make_channel<bool>(networkWorker, {.capacity_ = 1})) {}
    void attach(ruvia::execution_handle businessWorker) {
        incoming.emplace(ruvia::make_channel<NativeMessage>(businessWorker, {.capacity_ = 1}));
        writeDone.emplace(ruvia::make_channel<bool>(businessWorker, {.capacity_ = 1}));
        attached.store(true);
        if (!initialized.first.send(true).accepted()) {
            incoming->first.close();
            writeDone->first.close();
            throw std::runtime_error("Native channel setup cancelled");
        }
    }
    ruvia::task<std::optional<NativeMessage>> read() {
        auto result = co_await incoming->second.receive();
        if (!result.has_value()) {
            co_return std::nullopt;
        }
        if (!readCredit.first.send(true).accepted()) {
            throw std::runtime_error("Native read credit rejected");
        }
        co_return std::move(result).take_value();
    }
    ruvia::task<void> binary(std::string_view bytes) {
        if (bytes.empty() || bytes.size() > protocol::kMaximumMessageBytes || !outgoing.first.send(std::string(bytes)).accepted()) {
            throw std::runtime_error("Native write rejected");
        }
        auto result = co_await writeDone->second.receive();
        if (!result.has_value() || !result.value()) {
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
    std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>> outgoing;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> readCredit, initialized;
    std::optional<std::pair<ruvia::channel_sender<NativeMessage>, ruvia::channel_receiver<NativeMessage>>> incoming;
    std::optional<std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>>> writeDone;
};
} // namespace ultraterimal::server
