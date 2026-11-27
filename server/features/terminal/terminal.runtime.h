#pragma once
#include <array>
#include <atomic>
#include <channel.h>
#include <cstddef>
#include <memory>
#include <string>
#include <task.h>

#include "modules/sessions/session.types.h"
#include "ultraterimal/protocol/desktop.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
enum class TerminalCloseReason {
    kServerStopped,
    kUserClosed,
    kDeviceDeleted,
    kPeerClosed,
    kPeerNonBinary,
    kReadFailed,
    kWriteFailed,
    kSetupTimeout,
    kPermissionRevoked,
    kAuthorizationCheckFailed,
    kAttachRejected,
    kMalformedMessage,
    kInvalidBinding,
    kInvalidDirection,
    kBudgetExceeded,
    kQueueClosed,
    kReadyQueueClosed,
};

class TerminalDelivery {
  public:
    ~TerminalDelivery();
    TerminalDelivery(const TerminalDelivery&) = delete;
    TerminalDelivery& operator=(const TerminalDelivery&) = delete;
    const std::string& bytes() const { return bytes_; }
    bool ready() const { return bytes_.empty(); }

  private:
    friend class TerminalSession;
    friend class TerminalEndpoint;
    TerminalDelivery() = default;
    std::string bytes_;
    std::shared_ptr<std::atomic<std::size_t>> budget_;
};

class TerminalEndpoint {
  public:
    ruvia::task<std::unique_ptr<TerminalDelivery>> receive(ruvia::stop_token stop);

  private:
    friend class TerminalSession;
    TerminalEndpoint(ruvia::worker_ref ioWorker, protocol::ChannelSource source);
    void close();
    protocol::ChannelSource source_;
    std::atomic_bool closed_{false};
    std::pair<ruvia::channel_sender<std::unique_ptr<TerminalDelivery>>, ruvia::channel_receiver<std::unique_ptr<TerminalDelivery>>> queue_;
};

class TerminalSession {
  public:
    explicit TerminalSession(ruvia::worker_ref worker);
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    std::shared_ptr<TerminalEndpoint> attach(const ChannelAuthorization& authorization, protocol::ChannelSource source, ruvia::worker_ref ioWorker = {});
    ruvia::task<bool> forward(std::shared_ptr<TerminalEndpoint> sender, wire::Envelope message, ruvia::stop_token stop = {});
    bool ready() const { return ready_.load(std::memory_order_acquire) && !closed_.load(std::memory_order_acquire); }
    bool closed() const { return closed_.load(std::memory_order_acquire); }
    void close(TerminalCloseReason reason = TerminalCloseReason::kServerStopped);

  private:
    static constexpr std::size_t kMaximumSessionBytes = 512 * 1024;
    ruvia::worker_ref worker_;
    std::shared_ptr<std::atomic<std::size_t>> budget_ = std::make_shared<std::atomic<std::size_t>>(0);
    std::array<std::shared_ptr<TerminalEndpoint>, 2> endpoints_;
    std::array<std::uint64_t, 2> incomingSequences_{0, 0};
    std::array<std::uint64_t, 2> outgoingSequences_{1, 1};
    ChannelAuthorization identity_{};
    bool bound_ = false;
    std::atomic_bool ready_{false};
    std::atomic_bool closed_{false};
    TerminalCloseReason closeReason_ = TerminalCloseReason::kServerStopped;
};
} // namespace ultraterimal::server
