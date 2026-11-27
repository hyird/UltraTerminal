#pragma once
#include <array>
#include <atomic>
#include <channel.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <one_shot.h>
#include <optional>
#include <string>
#include <string_view>
#include <task.h>
#include <utility>

#include "desktop_diagnostics.types.h"
#include "modules/sessions/session.types.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::server {
enum class DesktopCloseReason {
    kServerStopped,
    kUserClosed,
    kPreempted,
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
    kReadOnlyInput,
    kMessageReplay,
    kInvalidFrameState,
    kSessionBudgetExceeded,
    kGlobalBudgetExceeded,
    kQueueClosed,
    kReadyQueueClosed,
};
enum class DesktopCloseOrigin { kServer,
                                kBrowserInput,
                                kAgentInput,
                                kBrowserVideo,
                                kAgentVideo };
// Cross-worker lock-free byte budget. Frame ownership includes the pending socket write.
struct DesktopBudget {
    explicit DesktopBudget(std::size_t maximum) : limit(maximum) {}
    bool tryReserve(std::size_t bytes) noexcept {
        auto current = used.load(std::memory_order_relaxed);
        while (bytes <= limit && current <= limit - bytes) {
            if (used.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
    void release(std::size_t bytes) noexcept {
        used.fetch_sub(bytes, std::memory_order_acq_rel);
    }
    const std::size_t limit;
    std::atomic<std::size_t> used{0};
};
class DesktopDelivery {
  public:
    ~DesktopDelivery();
    DesktopDelivery(const DesktopDelivery&) = delete;
    DesktopDelivery& operator=(const DesktopDelivery&) = delete;
    const std::string& bytes() const { return bytes_; }
    bool ready() const { return bytes_.empty(); }

  private:
    friend class DesktopSession;
    friend class DesktopEndpoint;
    DesktopDelivery() = default;
    std::string bytes_;
    std::shared_ptr<DesktopBudget> local_;
    std::shared_ptr<DesktopBudget> global_;
    bool browserVideo_ = false;
    bool primaryVideo_ = false;
    std::chrono::steady_clock::time_point enqueued_{}, received_{};
};
class DesktopEndpoint {
  public:
    ruvia::task<std::unique_ptr<DesktopDelivery>> receive(ruvia::stop_token stop);

  private:
    friend class DesktopSession;
    DesktopEndpoint(ruvia::worker_ref sessionWorker, ruvia::worker_ref ioWorker, protocol::DesktopChannelBinding binding);
    void close();
    std::atomic_bool closed_{false};
    std::atomic_bool waiting_{false};
    protocol::DesktopChannelValidator validator_;
    std::pair<ruvia::channel_sender<std::unique_ptr<DesktopDelivery>>, ruvia::channel_receiver<std::unique_ptr<DesktopDelivery>>> queue_;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> space_;
};
struct DesktopWriteTiming {
    std::chrono::steady_clock::time_point enqueued{};
    std::chrono::steady_clock::time_point received{};
    std::chrono::steady_clock::duration elapsed{};
    bool browserVideo = false;
    bool primaryVideo = false;
};
class DesktopSession {
  public:
    DesktopSession(ruvia::worker_ref worker, std::shared_ptr<DesktopBudget> global);
    ~DesktopSession();
    DesktopSession(const DesktopSession&) = delete;
    DesktopSession& operator=(const DesktopSession&) = delete;
    // Authorization is obtained from SessionService before attaching; no ticket is stored here.
    std::shared_ptr<DesktopEndpoint> attach(const ChannelAuthorization& authorization, protocol::ChannelSource source, ruvia::worker_ref ioWorker = {});
    // One reader per endpoint awaits admission before reading its next message.
    ruvia::task<bool> forward(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::stop_token stop = {});
    bool ready() const { return ready_.load(std::memory_order_acquire) && !closed_.load(std::memory_order_acquire); }
    bool closed() const { return closed_.load(std::memory_order_acquire); }
    void close(DesktopCloseReason reason = DesktopCloseReason::kServerStopped, DesktopCloseOrigin origin = DesktopCloseOrigin::kServer);
    DesktopCloseReason closeReason() const { return closeReason_; }
    DesktopCloseOrigin closeOrigin() const { return closeOrigin_; }
    const DesktopDiagnostics& diagnostics() const { return diagnostics_; }
    void logHealth();
    static DesktopWriteTiming captureWriteTiming(const DesktopDelivery& delivery, std::chrono::steady_clock::duration elapsed);
    // Only completed browser video writes are sampled; never diagnostics/control.
    void writeCompleted(const DesktopDelivery& delivery, std::chrono::steady_clock::duration elapsed);
    void writeCompleted(const DesktopWriteTiming& timing);

  private:
    void logDiagnostics(std::string_view event) const;
    void logCadence(std::chrono::steady_clock::time_point now);
    void optimizationBarrier(const wire::Envelope& message);
    std::uint32_t browserOptimizationRequest_ = 0, agentOptimizationCapability_ = 0;
    bool optimizationActive_ = false;
    ruvia::worker_ref worker_;
    std::shared_ptr<DesktopBudget> global_;
    std::shared_ptr<DesktopBudget> local_ = std::make_shared<DesktopBudget>(32 * 1024 * 1024);
    std::array<std::shared_ptr<DesktopEndpoint>, 4> endpoints_;
    std::array<std::string, 2> channels_;
    ChannelAuthorization identity_{};
    bool bound_ = false;
    std::atomic_bool ready_{false};
    std::atomic_bool closed_{false};
    DesktopCloseReason closeReason_ = DesktopCloseReason::kServerStopped;
    DesktopCloseOrigin closeOrigin_ = DesktopCloseOrigin::kServer;
    DesktopDiagnostics diagnostics_;
    DesktopCadenceWindow cadence_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};
} // namespace ultraterimal::server
