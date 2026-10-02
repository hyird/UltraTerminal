#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Channel.h>

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
    DesktopDelivery() = default;
    std::string bytes_;
    std::shared_ptr<DesktopBudget> local_;
    std::shared_ptr<DesktopBudget> global_;
};
class DesktopEndpoint {
  public:
    ruvia::Task<std::unique_ptr<DesktopDelivery>> receive(ruvia::StopToken stop);

  private:
    friend class DesktopSession;
    DesktopEndpoint(ruvia::WorkerHandle worker, protocol::DesktopChannelBinding binding);
    void close();
    bool closed_ = false;
    bool waiting_ = false;
    protocol::DesktopChannelValidator validator_;
    std::pair<ruvia::ChannelSender<std::unique_ptr<DesktopDelivery>>, ruvia::ChannelReceiver<std::unique_ptr<DesktopDelivery>>> queue_;
    std::pair<ruvia::ChannelSender<bool>, ruvia::ChannelReceiver<bool>> space_;
};
class DesktopSession {
  public:
    DesktopSession(ruvia::WorkerHandle worker, std::shared_ptr<DesktopBudget> global);
    ~DesktopSession();
    DesktopSession(const DesktopSession&) = delete;
    DesktopSession& operator=(const DesktopSession&) = delete;
    // Authorization is obtained from SessionService before attaching; no ticket is stored here.
    std::shared_ptr<DesktopEndpoint> attach(const ChannelAuthorization& authorization, protocol::ChannelSource source);
    // One reader per endpoint awaits admission before reading its next message.
    ruvia::Task<bool> forward(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::StopToken stop = {});
    bool ready() const { return ready_ && !closed_; }
    bool closed() const { return closed_; }
    void close(DesktopCloseReason reason = DesktopCloseReason::kServerStopped, DesktopCloseOrigin origin = DesktopCloseOrigin::kServer);
    DesktopCloseReason closeReason() const { return closeReason_; }
    DesktopCloseOrigin closeOrigin() const { return closeOrigin_; }
    const DesktopDiagnostics& diagnostics() const { return diagnostics_; }
    void logHealth();

  private:
    void logDiagnostics(std::string_view event) const;
    ruvia::WorkerHandle worker_;
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
