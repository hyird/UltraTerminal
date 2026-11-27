#include "control.runtime.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace ultraterimal::server {
ControlConnection::ControlConnection(ruvia::worker_ref worker) : worker_(worker), queue_(ruvia::make_channel<std::string>(std::move(worker), {.capacity_ = 32})) {}
ruvia::task<std::optional<wire::WindowsLoginResult>> ControlConnection::windowsLogin(wire::WindowsLoginRequest request) {
    if (!ready || !capabilities.windows_login_settings() || loginPending_ || loginRequest_ == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::nullopt;
    }
    auto [completion, receiver] = ruvia::make_one_shot<wire::WindowsLoginResult>(worker_);
    loginPending_ = completion;
    struct Pending {
        std::optional<ruvia::one_shot_completion<wire::WindowsLoginResult>>& value;
        ~Pending() { value.reset(); }
    } pending{loginPending_};
    request.set_request_id(++loginRequest_);
    wire::Envelope envelope;
    envelope.mutable_windows_login_request()->Swap(&request);
    if (!enqueue(std::move(envelope))) {
        co_return std::nullopt;
    }
    auto result = co_await receiver.wait({.timeout_ = std::chrono::seconds(5)});
    if (!result.has_value()) {
        co_return std::nullopt;
    }
    co_return std::move(result).take_value();
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
ruvia::task<std::optional<wire::FileListResult>> ControlConnection::fileList(wire::FileListRequest request) {
    if (!ready || !capabilities.file_transfer() || fileListPending_.size() >= 16 || fileListRequest_ == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::nullopt;
    }
    const auto id = ++fileListRequest_;
    auto [completion, receiver] = ruvia::make_one_shot<wire::FileListResult>(worker_);
    fileListPending_.emplace(id, std::move(completion));
    struct Pending {
        std::unordered_map<std::uint64_t, ruvia::one_shot_completion<wire::FileListResult>>& map;
        std::uint64_t id;
        ~Pending() { map.erase(id); }
    } pending{fileListPending_, id};
    request.set_request_id(id);
    wire::Envelope envelope;
    envelope.mutable_file_list_request()->Swap(&request);
    if (!enqueue(std::move(envelope))) {
        co_return std::nullopt;
    }
    auto result = co_await receiver.wait({.timeout_ = std::chrono::seconds(10)});
    if (!result.has_value()) {
        co_return std::nullopt;
    }
    co_return std::move(result).take_value();
}
bool ControlConnection::fileListReply(const wire::FileListResult& result) {
    if (!result.request_id() || result.request_id() > fileListRequest_) {
        return false;
    }
    if (const auto it = fileListPending_.find(result.request_id()); it != fileListPending_.end()) {
        auto completion = std::move(it->second);
        fileListPending_.erase(it);
        (void)completion.complete(result);
    }
    return true;
}
ruvia::task<std::optional<wire::FileStatResult>> ControlConnection::fileStat(wire::FileStatRequest request) {
    if (!ready || !capabilities.file_transfer() || fileStatPending_.size() >= 16 || fileStatRequest_ == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::nullopt;
    }
    const auto id = ++fileStatRequest_;
    auto [completion, receiver] = ruvia::make_one_shot<wire::FileStatResult>(worker_);
    fileStatPending_.emplace(id, std::move(completion));
    struct Pending {
        std::unordered_map<std::uint64_t, ruvia::one_shot_completion<wire::FileStatResult>>& map;
        std::uint64_t id;
        ~Pending() { map.erase(id); }
    } pending{fileStatPending_, id};
    request.set_request_id(id);
    wire::Envelope envelope;
    envelope.mutable_file_stat_request()->Swap(&request);
    if (!enqueue(std::move(envelope))) {
        co_return std::nullopt;
    }
    auto result = co_await receiver.wait({.timeout_ = std::chrono::seconds(10)});
    if (!result.has_value()) {
        co_return std::nullopt;
    }
    co_return std::move(result).take_value();
}
bool ControlConnection::fileStatReply(const wire::FileStatResult& result) {
    if (!result.request_id() || result.request_id() > fileStatRequest_) {
        return false;
    }
    if (const auto it = fileStatPending_.find(result.request_id()); it != fileStatPending_.end()) {
        auto completion = std::move(it->second);
        fileStatPending_.erase(it);
        (void)completion.complete(result);
    }
    return true;
}
bool ControlConnection::enqueueFileTransfer(wire::FileTransferStart request) {
    if (!ready || !capabilities.file_transfer()) {
        return false;
    }
    wire::Envelope envelope;
    envelope.mutable_file_transfer_start()->Swap(&request);
    return enqueue(std::move(envelope));
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
ruvia::task<std::optional<std::string>> ControlConnection::receive(ruvia::stop_token stop) {
    auto result = co_await queue_.second.receive({.stop_token_ = std::move(stop)});
    if (!result.has_value()) {
        co_return std::nullopt;
    }
    auto bytes = std::move(result).take_value();
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
    for (auto& [id, completion] : fileListPending_) {
        wire::FileListResult result;
        result.set_request_id(id);
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_CANCELLED);
        (void)completion.complete(std::move(result));
    }
    fileListPending_.clear();
    for (auto& [id, completion] : fileStatPending_) {
        wire::FileStatResult result;
        result.set_request_id(id);
        result.set_success(false);
        result.set_error(wire::FILE_ERROR_CANCELLED);
        (void)completion.complete(std::move(result));
    }
    fileStatPending_.clear();
}
} // namespace ultraterimal::server
