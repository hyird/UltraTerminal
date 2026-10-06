#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/channel.h>

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
// Owned and accessed on one Worker. Frame ownership includes the pending socket write.
struct DesktopBudget {
    explicit DesktopBudget(std::size_t maximum) : limit(maximum) {}
    const std::size_t limit;
    std::size_t used = 0;
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
    std::chrono::steady_clock::time_point enqueued_{}, received_{};
};
class DesktopEndpoint {
  public:
    ruvia::task<std::unique_ptr<DesktopDelivery>> receive(ruvia::stop_token stop);

  private:
    friend class DesktopSession;
    DesktopEndpoint(ruvia::execution_handle worker, protocol::DesktopChannelBinding binding);
    ruvia::task<std::unique_ptr<DesktopDelivery>> receiveOwned(ruvia::stop_token stop);
    void close();
    bool closed_ = false;
    bool waiting_ = false;
    protocol::DesktopChannelValidator validator_;
    std::pair<ruvia::channel_sender<std::unique_ptr<DesktopDelivery>>, ruvia::channel_receiver<std::unique_ptr<DesktopDelivery>>> queue_;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> space_;
};
class DesktopSession {
  public:
    DesktopSession(ruvia::execution_handle worker, std::shared_ptr<DesktopBudget> global);
    ~DesktopSession();
    DesktopSession(const DesktopSession&) = delete;
    DesktopSession& operator=(const DesktopSession&) = delete;
    // Authorization is obtained from SessionService before attaching; no ticket is stored here.
    std::shared_ptr<DesktopEndpoint> attach(const ChannelAuthorization& authorization, protocol::ChannelSource source);
    // One reader per endpoint awaits admission before reading its next message.
    ruvia::task<bool> forward(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::stop_token stop = {});
    bool ready() const { return ready_ && !closed_; }
    bool closed() const { return closed_; }
    void close(DesktopCloseReason reason = DesktopCloseReason::kServerStopped, DesktopCloseOrigin origin = DesktopCloseOrigin::kServer);
    DesktopCloseReason closeReason() const { return closeReason_; }
    DesktopCloseOrigin closeOrigin() const { return closeOrigin_; }
    const DesktopDiagnostics& diagnostics() const { return diagnostics_; }
    void logHealth();
    // Only completed browser video writes are sampled; never diagnostics/control.
    void writeCompleted(const DesktopDelivery& delivery, std::chrono::steady_clock::duration elapsed);

  private:
    ruvia::task<bool> forwardOwned(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::stop_token stop);
    void logDiagnostics(std::string_view event) const;
    void optimizationBarrier(const wire::Envelope& message);
    void completeRefinement();
    enum class RefinementAckResult { kForward,
                                     kIgnore,
                                     kInvalid };
    RefinementAckResult refinementAck(const wire::Envelope& message);
    struct RefinementRecord {
        DesktopFrameKey key;
        std::chrono::steady_clock::time_point sent;
    };
    // Sent auxiliary lease: only exact cleanup ACK or session end completes it.
    // Config/frame retirement and timeout must never manufacture new credit.
    std::optional<RefinementRecord> outstandingRefinement_;
    std::array<DesktopFrameKey, 128> retiredRefinements_{};
    std::size_t nextRetiredRefinement_ = 0;
    DesktopFrameKey evictedRefinement_{};
    std::uint64_t pausedRefinementGeneration_ = 0;
    std::uint32_t browserOptimizationRequest_ = 0, agentOptimizationCapability_ = 0;
    bool optimizationActive_ = false;
    DesktopFrameKey cumulativeVideoStages_{};
    ruvia::execution_handle worker_;
    std::shared_ptr<DesktopBudget> global_;
    std::shared_ptr<DesktopBudget> local_ = std::make_shared<DesktopBudget>(32 * 1024 * 1024);
    std::array<std::shared_ptr<DesktopEndpoint>, 4> endpoints_;
    std::array<std::string, 2> channels_;
    ChannelAuthorization identity_{};
    bool bound_ = false;
    bool ready_ = false;
    bool closed_ = false;
    DesktopCloseReason closeReason_ = DesktopCloseReason::kServerStopped;
    DesktopCloseOrigin closeOrigin_ = DesktopCloseOrigin::kServer;
    DesktopDiagnostics diagnostics_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};
} // namespace ultraterimal::server
