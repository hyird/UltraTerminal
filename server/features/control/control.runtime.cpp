#include "control.runtime.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace ultraterimal::server {
ControlConnection::ControlConnection(ruvia::WorkerHandle worker) : worker_(worker), queue_(ruvia::makeChannel<std::string>(std::move(worker), {.capacity = 32})) {}
ruvia::Task<std::optional<wire::WindowsLoginResult>> ControlConnection::windowsLogin(wire::WindowsLoginRequest request) {
    if (!ready || !capabilities.windows_login_settings() || loginPending_ || loginRequest_ == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::nullopt;
    }
    auto [completion, receiver] = ruvia::makeOneShot<wire::WindowsLoginResult>(worker_);
    loginPending_ = completion;
    struct Pending {
        std::optional<ruvia::OneShotCompletion<wire::WindowsLoginResult>>& value;
        ~Pending() { value.reset(); }
    } pending{loginPending_};
    request.set_request_id(++loginRequest_);
    wire::Envelope envelope;
    envelope.mutable_windows_login_request()->Swap(&request);
    if (!enqueue(std::move(envelope))) {
        co_return std::nullopt;
    }
    auto result = co_await receiver.waitFor(std::chrono::seconds(5));
    if (!result.hasValue()) {
        co_return std::nullopt;
    }
    co_return std::move(result).takeValue();
}
bool ControlConnection::windowsLoginReply(const wire::WindowsLoginResult& result) {
    if (!result.request_id() || result.request_id() > loginRequest_) {
        return false;
    }
    if (loginPending_ && result.request_id() == loginRequest_) {
        (void)loginPending_->complete(result);
    }
    return true;
}
bool ControlConnection::enqueueRestart(wire::WindowsRestartRequest request) {
    if (!ready || !capabilities.system_restart() || request.operation_id().empty() || sentRestartOperations_.size() >= 128 ||
        std::find(sentRestartOperations_.begin(), sentRestartOperations_.end(), request.operation_id()) != sentRestartOperations_.end()) {
        return false;
    }
    // Reserve history before queue acceptance, so allocation failure cannot leave an
    // accepted command without a correlation record. Roll back only on definite reject.
    sentRestartOperations_.push_back(request.operation_id());
    try {
        wire::Envelope envelope;
        envelope.mutable_windows_restart_request()->Swap(&request);
        if (!enqueue(std::move(envelope))) {
            sentRestartOperations_.pop_back();
            return false;
        }
    } catch (...) {
        sentRestartOperations_.pop_back();
        return false;
    }
    return true;
}
bool ControlConnection::windowsRestartReply(const wire::WindowsRestartResult& result) {
    if (result.operation_id().empty() || !wire::WindowsRestartResult_Status_IsValid(result.status()) || result.status() == wire::WindowsRestartResult::UNSPECIFIED) {
        return false;
    }
    // Late and repeated results remain valid only while the operation is in this
    // authenticated connection's bounded dispatch history.
    return std::find(sentRestartOperations_.begin(), sentRestartOperations_.end(), result.operation_id()) != sentRestartOperations_.end();
}
std::vector<std::string> ControlConnection::restartOperations() const {
    return {sentRestartOperations_.begin(), sentRestartOperations_.end()};
}
bool ControlConnection::enqueue(wire::Envelope message) {
    if (!ready || sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    message.set_protocol_version(protocol::kVersion);
    message.set_sequence(sequence_ + 1);
    auto bytes = protocol::encodeMessage(message);
    if (!bytes || bytes->size() > protocol::kMaximumControlBytes || queuedBytes_ + bytes->size() > 256 * 1024) {
        return false;
    }
    const auto size = bytes->size();
    if (!queue_.first.send(std::move(*bytes)).accepted()) {
        return false;
    }
    queuedBytes_ += size;
    ++sequence_;
    return true;
}
ruvia::Task<std::optional<std::string>> ControlConnection::receive(ruvia::StopToken stop) {
    auto result = co_await queue_.second.receive(std::move(stop));
    if (!result.hasValue()) {
        co_return std::nullopt;
    }
    auto bytes = std::move(result).takeValue();
    queuedBytes_ -= bytes.size();
    co_return bytes;
}
void ControlConnection::close() {
    ready = false;
    queue_.first.close();
    if (loginPending_) {
        wire::WindowsLoginResult result;
        result.set_request_id(loginRequest_);
        (void)loginPending_->complete(std::move(result));
    }
}
} // namespace ultraterimal::server
