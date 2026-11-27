#include "features/file/file.runtime.h"

#include <array>
#include <stdexcept>
#include <task_scope.h>

#include <openssl/rand.h>

#include "config/server.runtime.h"

namespace ultraterimal::server {
namespace {
std::string generateTransferId() {
    std::array<unsigned char, 32> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("Cannot generate file transfer id");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(64);
    for (const auto byte : raw) {
        id.push_back(kHex[byte >> 4]);
        id.push_back(kHex[byte & 0x0f]);
    }
    return id;
}
} // namespace

std::shared_ptr<FileTransferBridge> FileTransferHub::create(const std::string& deviceId, wire::FileTransferStart::Direction direction, ruvia::worker_ref httpWorker) {
    std::lock_guard lock(mutex_);
    if (pending_.size() >= 64) {
        return nullptr;
    }
    std::string id = generateTransferId();
    auto bridge = std::make_shared<FileTransferBridge>(id, deviceId, direction, std::move(httpWorker));
    pending_.emplace(std::move(id), bridge);
    return bridge;
}

std::shared_ptr<FileTransferBridge> FileTransferHub::pair(const std::string& transferId, const std::string& deviceId, ruvia::worker_ref agentWorker) {
    std::shared_ptr<FileTransferBridge> bridge;
    {
        std::lock_guard lock(mutex_);
        const auto it = pending_.find(transferId);
        if (it == pending_.end()) {
            return nullptr;
        }
        bridge = it->second;
        pending_.erase(it);
    }
    if (bridge->deviceId != deviceId) {
        bridge->close();
        return nullptr;
    }
    std::optional<ruvia::one_shot_completion<bool>> completion;
    {
        std::lock_guard lock(bridge->mutex);
        if (bridge->closed || bridge->paired) {
            return nullptr;
        }
        bridge->paired = true;
        bridge->toAgent.emplace(ruvia::make_channel<std::string>(std::move(agentWorker), {.capacity_ = 8}));
        completion = std::move(bridge->connectedCompletion);
    }
    if (completion && completion->valid()) {
        (void)completion->complete(true);
    }
    return bridge;
}

void FileTransferHub::remove(const std::string& transferId) {
    std::shared_ptr<FileTransferBridge> bridge;
    {
        std::lock_guard lock(mutex_);
        if (const auto it = pending_.find(transferId); it != pending_.end()) {
            bridge = std::move(it->second);
            pending_.erase(it);
        }
    }
    if (bridge) {
        bridge->close();
    }
}
} // namespace ultraterimal::server
