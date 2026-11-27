#pragma once
#include <channel.h>
#include <memory>
#include <mutex>
#include <one_shot.h>
#include <optional>
#include <string>
#include <task.h>
#include <task_scope.h>
#include <unordered_map>

#include "features/native/native.transport.h"
#include "ultraterimal/protocol/file.protocol.h"

namespace ultraterimal::server {
struct FileTransferBridge {
    std::string transferId;
    std::string deviceId;
    wire::FileTransferStart::Direction direction = wire::FileTransferStart::UNSPECIFIED;
    std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>> toHttp;
    std::optional<std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>>> toAgent;
    std::optional<ruvia::one_shot_completion<bool>> connectedCompletion;
    std::optional<ruvia::one_shot_receiver<bool>> connectedReceiver;
    std::mutex mutex;
    bool paired = false;
    bool closed = false;

    FileTransferBridge(std::string id, std::string device, wire::FileTransferStart::Direction dir, ruvia::worker_ref httpWorker)
        : transferId(std::move(id)),
          deviceId(std::move(device)),
          direction(dir),
          toHttp(ruvia::make_channel<std::string>(httpWorker, {.capacity_ = 8})) {
        auto [completion, receiver] = ruvia::make_one_shot<bool>(httpWorker);
        connectedCompletion.emplace(std::move(completion));
        connectedReceiver.emplace(std::move(receiver));
    }

    void close() {
        std::optional<ruvia::one_shot_completion<bool>> pending;
        {
            std::lock_guard lock(mutex);
            if (closed) {
                return;
            }
            closed = true;
            pending = std::move(connectedCompletion);
            if (toAgent) {
                toAgent->first.close();
            }
        }
        toHttp.first.close();
        if (pending && pending->valid()) {
            (void)pending->complete(false);
        }
    }
};

class FileTransferHub {
  public:
    std::shared_ptr<FileTransferBridge> create(const std::string& deviceId, wire::FileTransferStart::Direction direction, ruvia::worker_ref httpWorker);
    std::shared_ptr<FileTransferBridge> pair(const std::string& transferId, const std::string& deviceId, ruvia::worker_ref agentWorker);
    void remove(const std::string& transferId);

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<FileTransferBridge>> pending_;
};

template <typename Context, typename Connection>
ruvia::task<void> serveFileTransfer(Context& context, Connection& socket, FileTransferHub& hub, const std::string& deviceId) {
    const auto first = co_await socket.read();
    if (!first || !first->binary()) {
        co_return;
    }
    const auto hello = protocol::decodeFileTransferFrame(first->payload());
    if (!hello || !hello->has_hello()) {
        co_return;
    }
    auto bridge = hub.pair(hello->hello().transfer_id(), deviceId, context.worker());
    if (!bridge) {
        co_return;
    }
    struct BridgeGuard {
        std::shared_ptr<FileTransferBridge> bridge;
        ~BridgeGuard() { bridge->close(); }
    } guard{bridge};

    ruvia::task_scope scope(context.worker());
    scope.spawn([](Connection* sock, std::shared_ptr<FileTransferBridge> br, ruvia::task_scope* sc) -> ruvia::task<void> {
        try {
            while (!sc->stop_requested()) {
                auto incoming = co_await br->toAgent->second.receive({.stop_token_ = sc->token()});
                if (!incoming.has_value()) {
                    break;
                }
                co_await sock->binary(std::move(incoming).take_value());
            }
        } catch (...) {
        }
        br->close();
        sock->abort();
        sc->request_stop();
    }(&socket, bridge, &scope));

    try {
        while (!scope.stop_requested()) {
            auto msg = co_await socket.read();
            if (!msg || !msg->binary()) {
                break;
            }
            std::string payload(msg->payload());
            const auto sent = co_await bridge->toHttp.first.send_wait(std::move(payload), {.stop_token_ = scope.token()});
            if (!sent.accepted()) {
                break;
            }
        }
    } catch (...) {
    }
    bridge->close();
    socket.abort();
    scope.request_stop();
    co_await scope.join();
}
} // namespace ultraterimal::server
