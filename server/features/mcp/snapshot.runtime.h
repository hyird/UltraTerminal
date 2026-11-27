#pragma once
#include <memory>
#include <mutex>
#include <one_shot.h>
#include <optional>
#include <string>
#include <task.h>
#include <unordered_map>

#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::server {
struct DesktopSnapshotWaiter {
    std::string snapshotId;
    std::string deviceId;
    std::optional<ruvia::one_shot_completion<wire::DesktopSnapshotResult>> completion;
    std::optional<ruvia::one_shot_receiver<wire::DesktopSnapshotResult>> receiver;
    std::mutex mutex;
    bool completed = false;

    DesktopSnapshotWaiter(std::string id, std::string device, ruvia::worker_ref httpWorker)
        : snapshotId(std::move(id)), deviceId(std::move(device)) {
        auto [comp, recv] = ruvia::make_one_shot<wire::DesktopSnapshotResult>(std::move(httpWorker));
        completion.emplace(std::move(comp));
        receiver.emplace(std::move(recv));
    }

    void finish(wire::DesktopSnapshotResult result) {
        std::optional<ruvia::one_shot_completion<wire::DesktopSnapshotResult>> pending;
        {
            std::lock_guard lock(mutex);
            if (completed) {
                return;
            }
            completed = true;
            pending = std::move(completion);
        }
        if (pending && pending->valid()) {
            (void)pending->complete(std::move(result));
        }
    }

    void cancel() {
        wire::DesktopSnapshotResult cancelled;
        cancelled.set_snapshot_id(snapshotId);
        cancelled.set_success(false);
        cancelled.set_error(wire::DESKTOP_TOOL_ERROR_FAILED);
        finish(std::move(cancelled));
    }
};

class DesktopSnapshotHub {
  public:
    std::shared_ptr<DesktopSnapshotWaiter> create(const std::string& deviceId, ruvia::worker_ref httpWorker);
    bool complete(const std::string& deviceId, wire::DesktopSnapshotResult result);
    void remove(const std::string& snapshotId);

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<DesktopSnapshotWaiter>> pending_;
};

template <typename Context, typename Connection>
ruvia::task<void> serveDesktopSnapshot(Context&, Connection& socket, DesktopSnapshotHub& hub, const std::string& deviceId) {
    const auto msg = co_await socket.read();
    if (!msg || !msg->binary()) {
        co_return;
    }
    auto parsed = protocol::decodeDesktopSnapshotResult(msg->payload());
    if (!parsed) {
        co_return;
    }
    (void)hub.complete(deviceId, std::move(*parsed));
}
} // namespace ultraterimal::server
