#include "features/mcp/snapshot.runtime.h"

#include <array>
#include <stdexcept>

#include <openssl/rand.h>

namespace ultraterimal::server {
namespace {
std::string generateSnapshotId() {
    std::array<unsigned char, 32> raw{};
    if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("Cannot generate snapshot id");
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

std::shared_ptr<DesktopSnapshotWaiter> DesktopSnapshotHub::create(const std::string& deviceId, ruvia::worker_ref httpWorker) {
    std::lock_guard lock(mutex_);
    if (pending_.size() >= 16) {
        return nullptr;
    }
    std::string id = generateSnapshotId();
    auto waiter = std::make_shared<DesktopSnapshotWaiter>(id, deviceId, std::move(httpWorker));
    pending_.emplace(std::move(id), waiter);
    return waiter;
}

bool DesktopSnapshotHub::complete(const std::string& deviceId, wire::DesktopSnapshotResult result) {
    std::shared_ptr<DesktopSnapshotWaiter> waiter;
    {
        std::lock_guard lock(mutex_);
        const auto it = pending_.find(result.snapshot_id());
        if (it == pending_.end()) {
            return false;
        }
        waiter = it->second;
        pending_.erase(it);
    }
    if (waiter->deviceId != deviceId) {
        waiter->cancel();
        return false;
    }
    waiter->finish(std::move(result));
    return true;
}

void DesktopSnapshotHub::remove(const std::string& snapshotId) {
    std::shared_ptr<DesktopSnapshotWaiter> waiter;
    {
        std::lock_guard lock(mutex_);
        if (const auto it = pending_.find(snapshotId); it != pending_.end()) {
            waiter = std::move(it->second);
            pending_.erase(it);
        }
    }
    if (waiter) {
        waiter->cancel();
    }
}
} // namespace ultraterimal::server
