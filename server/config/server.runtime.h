#pragma once
#include <app.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <one_shot.h>
#include <optional>
#include <string>
#include <string_view>
#include <task.h>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "features/control/control.runtime.h"
#include "features/desktop/desktop.runtime.h"
#include "features/status/status.runtime.h"
#include "modules/devices/device.service.h"
#include "modules/devices/update.service.h"
#include "modules/identity/identity.service.h"
#include "modules/sessions/session.service.h"
#include "modules/telegram/telegram.service.h"
#include "server.config.h"

namespace ultraterimal::server {
struct ServerSharedState {
    std::shared_ptr<DesktopBudget> desktopBudget = std::make_shared<DesktopBudget>(256 * 1024 * 1024);
    std::shared_ptr<std::atomic_bool> telegramConfiguring = std::make_shared<std::atomic_bool>(false);
};
struct ServerRuntime {
    ServerConfig config;
    std::shared_ptr<ServerSharedState> shared;
    IdentityService identities;
    DeviceService devices;
    SessionService sessions;
    TelegramService telegram;
    StatusHub status;
    // Reserve the device during handshake; only a validated HELLO makes it ready.
    std::unordered_map<std::string, std::shared_ptr<ControlConnection>> controlConnections;
    std::unordered_map<std::string, std::shared_ptr<DesktopSession>> desktopSessions;

    explicit ServerRuntime(ServerConfig value, std::shared_ptr<ServerSharedState> sharedState = std::make_shared<ServerSharedState>())
        : config(std::move(value)),
          shared(sharedState ? std::move(sharedState) : std::make_shared<ServerSharedState>()),
          identities(config.database, config.securityKey),
          devices(config.database),
          sessions(config.database, config.securityKey),
          telegram(config.database, config.securityKey) {}
};

struct DeviceOnlineState {
    bool online = false;
    std::string agentVersion;
};
struct DeviceSessionCapabilities {
    bool ready = false;
    bool inputSupported = false;
    bool requestedSupported = false;
};
enum class DeviceUpdateDispatch { kOffline,
                                  kCurrent,
                                  kRequested,
                                  kBusy };
struct DeviceUpdateResult {
    DeviceUpdateDispatch status = DeviceUpdateDispatch::kOffline;
    std::string version;
};
struct DeviceRestartCapability {
    bool ready = false;
    std::uint64_t bootTimeUnixSeconds = 0;
};

template <typename T>
struct WorkerRpcOutcome {
    using Storage = std::conditional_t<std::is_void_v<T>, std::monostate, std::optional<T>>;
    [[no_unique_address]] Storage value{};
    std::exception_ptr failure;
    bool aborted = false;
};

template <typename T>
class WorkerRpcGuard {
  public:
    explicit WorkerRpcGuard(ruvia::one_shot_completion<WorkerRpcOutcome<T>> completion) noexcept
        : completion_(std::move(completion)) {}
    WorkerRpcGuard(const WorkerRpcGuard&) = delete;
    WorkerRpcGuard& operator=(const WorkerRpcGuard&) = delete;
    WorkerRpcGuard(WorkerRpcGuard&&) noexcept = default;
    WorkerRpcGuard& operator=(WorkerRpcGuard&&) = delete;
    ~WorkerRpcGuard() {
        if (completion_.valid()) {
            WorkerRpcOutcome<T> aborted;
            aborted.aborted = true;
            (void)completion_.complete(std::move(aborted));
        }
    }
    void answer(WorkerRpcOutcome<T> outcome) {
        const auto target = std::move(completion_);
        (void)target.complete(std::move(outcome));
    }

  private:
    ruvia::one_shot_completion<WorkerRpcOutcome<T>> completion_;
};

ruvia::web_worker_ref resolveWebWorker(std::string_view key);

template <typename T, typename Fn>
ruvia::task<std::optional<T>> callOnWebWorker(const ruvia::worker_ref& caller, ServerRuntime& local, const ruvia::web_worker_ref& target, Fn&& fn) {
    if (!target.valid() || target.is_current()) {
        co_return fn(local);
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<T>>(caller);
    const auto posted = target.post([guard = WorkerRpcGuard<T>(std::move(completion)), call = std::forward<Fn>(fn)](ruvia::web_worker_context& ctx) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<T> out;
        try {
            out.value.emplace(call(ctx.worker_state<ServerRuntime>()));
        } catch (...) {
            out.failure = std::current_exception();
        }
        guard.answer(std::move(out));
        co_return;
    });
    if (!posted.accepted()) {
        co_return std::nullopt;
    }
    auto waited = co_await receiver.wait();
    if (!waited.has_value()) {
        co_return std::nullopt;
    }
    auto outcome = std::move(waited).take_value();
    if (outcome.failure) {
        std::rethrow_exception(outcome.failure);
    }
    if (outcome.aborted) {
        co_return std::nullopt;
    }
    co_return std::move(*outcome.value);
}

void publishClusterStatus(ServerRuntime& local, const std::string& event);
ruvia::task<DeviceOnlineState> queryDeviceOnlineState(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId);
ruvia::task<std::unordered_map<std::string, DeviceOnlineState>> queryDeviceOnlineStates(const ruvia::worker_ref& caller, ServerRuntime& local, const std::vector<std::string>& deviceIds);
void closeDeviceControl(ServerRuntime& local, const std::string& deviceId);
void sendDeviceSessionClose(ServerRuntime& local, const std::string& deviceId, const std::string& sessionId, std::string reason);
ruvia::task<DeviceUpdateResult> dispatchDeviceUpdate(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, const std::string& userId, const AgentRelease& release);
ruvia::task<std::optional<wire::WindowsLoginResult>> requestDeviceWindowsLogin(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::WindowsLoginRequest request);
ruvia::task<DeviceRestartCapability> queryDeviceRestartCapability(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId);
ruvia::task<bool> enqueueDeviceRestart(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::WindowsRestartRequest request);
ruvia::task<DeviceSessionCapabilities> queryDeviceSessionCapabilities(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::SessionKind kind, bool writeControl, std::uint32_t inputContract);
ruvia::task<bool> dispatchSessionOpen(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::Envelope openCommand, const std::vector<std::string>& preemptedSessions);
void closeDesktopSession(ServerRuntime& local, const std::string& sessionId, DesktopCloseReason reason, DesktopCloseOrigin origin = DesktopCloseOrigin::kServer);
} // namespace ultraterimal::server
