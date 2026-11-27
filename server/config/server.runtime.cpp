#include "server.runtime.h"

#include <spdlog/spdlog.h>

namespace ultraterimal::server {
namespace {
DeviceOnlineState makeOnlineState(const ControlConnection& conn) {
    return DeviceOnlineState{
        true,
        conn.agentVersion,
        conn.platform,
        conn.capabilities.capture_available(),
        conn.capabilities.terminal() || (conn.capabilities.ssh() && conn.capabilities.conpty()),
        conn.capabilities.windows_login_settings(),
    };
}
} // namespace

ruvia::web_worker_ref resolveWebWorker(std::string_view key) {
    const auto workers = ruvia::app().worker_refs();
    if (workers.empty()) {
        return {};
    }
    return ruvia::app().worker_for(key);
}

void publishClusterStatus(ServerRuntime& local, const std::string& event) {
    const auto workers = ruvia::app().worker_refs();
    if (workers.empty()) {
        local.status.publish(event);
        return;
    }
    for (const auto& worker : workers) {
        if (worker.is_current()) {
            local.status.publish(event);
        } else {
            (void)worker.post([event](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
                ctx.worker_state<ServerRuntime>().status.publish(event);
                co_return;
            });
        }
    }
}

ruvia::task<DeviceOnlineState> queryDeviceOnlineState(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<DeviceOnlineState>(caller, local, target, [deviceId](ServerRuntime& rt) {
        const auto it = rt.controlConnections.find(deviceId);
        if (it == rt.controlConnections.end() || !it->second->ready) {
            return DeviceOnlineState{};
        }
        return makeOnlineState(*it->second);
    });
    co_return result.value_or(DeviceOnlineState{});
}

ruvia::task<std::unordered_map<std::string, DeviceOnlineState>> queryDeviceOnlineStates(const ruvia::worker_ref& caller, ServerRuntime& local, const std::vector<std::string>& deviceIds) {
    std::unordered_map<std::string, DeviceOnlineState> states;
    const auto workers = ruvia::app().worker_refs();
    if (workers.size() <= 1) {
        for (const auto& id : deviceIds) {
            const auto it = local.controlConnections.find(id);
            if (it != local.controlConnections.end() && it->second->ready) {
                states.emplace(id, makeOnlineState(*it->second));
            }
        }
        co_return states;
    }
    std::unordered_map<ruvia::worker_id_type, std::pair<ruvia::web_worker_ref, std::vector<std::string>>> buckets;
    for (const auto& id : deviceIds) {
        auto worker = ruvia::app().worker_for(id);
        auto& bucket = buckets[worker.id()];
        bucket.first = std::move(worker);
        bucket.second.push_back(id);
    }
    for (const auto& [workerId, bucket] : buckets) {
        auto partial = co_await callOnWebWorker<std::vector<std::pair<std::string, DeviceOnlineState>>>(
            caller, local, bucket.first, [ids = bucket.second](ServerRuntime& rt) {
                std::vector<std::pair<std::string, DeviceOnlineState>> out;
                out.reserve(ids.size());
                for (const auto& id : ids) {
                    const auto it = rt.controlConnections.find(id);
                    if (it != rt.controlConnections.end() && it->second->ready) {
                        out.emplace_back(id, makeOnlineState(*it->second));
                    }
                }
                return out;
            });
        if (partial) {
            for (auto& [id, st] : *partial) {
                states.emplace(std::move(id), std::move(st));
            }
        }
    }
    co_return states;
}

void closeDeviceControl(ServerRuntime& local, const std::string& deviceId) {
    const auto target = resolveWebWorker(deviceId);
    if (!target.valid() || target.is_current()) {
        if (const auto it = local.controlConnections.find(deviceId); it != local.controlConnections.end()) {
            it->second->close();
        }
        return;
    }
    (void)target.post([deviceId](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
        auto& rt = ctx.worker_state<ServerRuntime>();
        if (const auto it = rt.controlConnections.find(deviceId); it != rt.controlConnections.end()) {
            it->second->close();
        }
        co_return;
    });
}

void sendDeviceSessionClose(ServerRuntime& local, const std::string& deviceId, const std::string& sessionId, std::string reason) {
    const auto sendLocal = [deviceId, sessionId, reason = std::move(reason)](ServerRuntime& rt) {
        const auto it = rt.controlConnections.find(deviceId);
        if (it == rt.controlConnections.end()) {
            return;
        }
        wire::Envelope command;
        command.set_session_id(sessionId);
        command.mutable_session_close()->set_reason(reason);
        if (!it->second->enqueue(std::move(command))) {
            it->second->close();
        }
    };
    const auto target = resolveWebWorker(deviceId);
    if (!target.valid() || target.is_current()) {
        sendLocal(local);
        return;
    }
    (void)target.post([sendLocal = std::move(sendLocal)](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
        sendLocal(ctx.worker_state<ServerRuntime>());
        co_return;
    });
}

ruvia::task<DeviceUpdateResult> dispatchDeviceUpdate(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, const std::string& userId, const AgentRelease& release) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<DeviceUpdateResult>(caller, local, target, [deviceId, userId, release](ServerRuntime& rt) {
        const auto found = rt.controlConnections.find(deviceId);
        if (found == rt.controlConnections.end() || !found->second->ready || !found->second->capabilities.agent_update()) {
            return DeviceUpdateResult{DeviceUpdateDispatch::kOffline, {}};
        }
        const auto connection = found->second;
        if (!protocol::newerUpdate(release.update, connection->agentVersion)) {
            return DeviceUpdateResult{DeviceUpdateDispatch::kCurrent, connection->agentVersion};
        }
        if (!connection->updatePending) {
            wire::Envelope command;
            *command.mutable_agent_update() = release.update;
            if (!connection->enqueue(std::move(command))) {
                return DeviceUpdateResult{DeviceUpdateDispatch::kBusy, {}};
            }
            connection->updatePending = true;
            spdlog::info("agent_update_requested user_id={} device_id={} version={}", userId, deviceId, release.update.version());
        }
        return DeviceUpdateResult{DeviceUpdateDispatch::kRequested, release.update.version()};
    });
    co_return result.value_or(DeviceUpdateResult{DeviceUpdateDispatch::kOffline, {}});
}

ruvia::task<std::optional<wire::WindowsLoginResult>> requestDeviceWindowsLogin(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::WindowsLoginRequest request) {
    const auto target = resolveWebWorker(deviceId);
    if (!target.valid() || target.is_current()) {
        const auto found = local.controlConnections.find(deviceId);
        if (found == local.controlConnections.end() || !found->second->ready || !found->second->capabilities.windows_login_settings()) {
            co_return std::nullopt;
        }
        co_return co_await found->second->windowsLogin(std::move(request));
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<std::optional<wire::WindowsLoginResult>>>(caller);
    const auto posted = target.post([guard = WorkerRpcGuard<std::optional<wire::WindowsLoginResult>>(std::move(completion)), deviceId, request = std::move(request)](ruvia::web_worker_context& ctx) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<std::optional<wire::WindowsLoginResult>> out;
        try {
            auto& rt = ctx.worker_state<ServerRuntime>();
            const auto found = rt.controlConnections.find(deviceId);
            if (found == rt.controlConnections.end() || !found->second->ready || !found->second->capabilities.windows_login_settings()) {
                out.value.emplace(std::nullopt);
            } else {
                out.value.emplace(co_await found->second->windowsLogin(std::move(request)));
            }
        } catch (...) {
            out.failure = std::current_exception();
        }
        guard.answer(std::move(out));
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
    if (outcome.aborted || !outcome.value) {
        co_return std::nullopt;
    }
    co_return std::move(*outcome.value);
}

ruvia::task<std::optional<wire::FileListResult>> requestDeviceFileList(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::FileListRequest request) {
    const auto target = resolveWebWorker(deviceId);
    if (!target.valid() || target.is_current()) {
        const auto found = local.controlConnections.find(deviceId);
        if (found == local.controlConnections.end() || !found->second->ready || !found->second->capabilities.file_transfer()) {
            co_return std::nullopt;
        }
        co_return co_await found->second->fileList(std::move(request));
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<std::optional<wire::FileListResult>>>(caller);
    const auto posted = target.post([guard = WorkerRpcGuard<std::optional<wire::FileListResult>>(std::move(completion)), deviceId, request = std::move(request)](ruvia::web_worker_context& ctx) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<std::optional<wire::FileListResult>> out;
        try {
            auto& rt = ctx.worker_state<ServerRuntime>();
            const auto found = rt.controlConnections.find(deviceId);
            if (found != rt.controlConnections.end() && found->second->ready && found->second->capabilities.file_transfer()) {
                out.value.emplace(co_await found->second->fileList(std::move(request)));
            } else {
                out.value.emplace(std::nullopt);
            }
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
    if (outcome.aborted || !outcome.value) {
        co_return std::nullopt;
    }
    co_return std::move(*outcome.value);
}

ruvia::task<std::optional<wire::FileStatResult>> requestDeviceFileStat(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::FileStatRequest request) {
    const auto target = resolveWebWorker(deviceId);
    if (!target.valid() || target.is_current()) {
        const auto found = local.controlConnections.find(deviceId);
        if (found == local.controlConnections.end() || !found->second->ready || !found->second->capabilities.file_transfer()) {
            co_return std::nullopt;
        }
        co_return co_await found->second->fileStat(std::move(request));
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<std::optional<wire::FileStatResult>>>(caller);
    const auto posted = target.post([guard = WorkerRpcGuard<std::optional<wire::FileStatResult>>(std::move(completion)), deviceId, request = std::move(request)](ruvia::web_worker_context& ctx) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<std::optional<wire::FileStatResult>> out;
        try {
            auto& rt = ctx.worker_state<ServerRuntime>();
            const auto found = rt.controlConnections.find(deviceId);
            if (found != rt.controlConnections.end() && found->second->ready && found->second->capabilities.file_transfer()) {
                out.value.emplace(co_await found->second->fileStat(std::move(request)));
            } else {
                out.value.emplace(std::nullopt);
            }
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
    if (outcome.aborted || !outcome.value) {
        co_return std::nullopt;
    }
    co_return std::move(*outcome.value);
}

ruvia::task<bool> dispatchDeviceFileTransfer(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::FileTransferStart request) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<bool>(caller, local, target, [deviceId, request = std::move(request)](ServerRuntime& rt) mutable {
        const auto found = rt.controlConnections.find(deviceId);
        if (found == rt.controlConnections.end() || !found->second->ready || !found->second->capabilities.file_transfer()) {
            return false;
        }
        return found->second->enqueueFileTransfer(std::move(request));
    });
    co_return result.value_or(false);
}

ruvia::task<DeviceRestartCapability> queryDeviceRestartCapability(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<DeviceRestartCapability>(caller, local, target, [deviceId](ServerRuntime& rt) {
        const auto current = rt.controlConnections.find(deviceId);
        if (current == rt.controlConnections.end() || !current->second->ready || !current->second->capabilities.system_restart()) {
            return DeviceRestartCapability{};
        }
        return DeviceRestartCapability{true, current->second->capabilities.boot_time_unix_seconds()};
    });
    co_return result.value_or(DeviceRestartCapability{});
}

ruvia::task<bool> enqueueDeviceRestart(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::WindowsRestartRequest request) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<bool>(caller, local, target, [deviceId, request = std::move(request)](ServerRuntime& rt) mutable {
        const auto current = rt.controlConnections.find(deviceId);
        return current != rt.controlConnections.end() && current->second->ready && current->second->enqueueRestart(std::move(request));
    });
    co_return result.value_or(false);
}

ruvia::task<DeviceSessionCapabilities> queryDeviceSessionCapabilities(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::SessionKind kind, bool writeControl, std::uint32_t inputContract) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<DeviceSessionCapabilities>(caller, local, target, [deviceId, kind, writeControl, inputContract](ServerRuntime& rt) {
        const auto found = rt.controlConnections.find(deviceId);
        if (found == rt.controlConnections.end() || !found->second->ready) {
            return DeviceSessionCapabilities{};
        }
        const auto& caps = found->second->capabilities;
        const bool inputSupported = caps.capture_available() && caps.desktop_input_contract() == 2;
        const bool terminalSupported = caps.terminal() || (caps.ssh() && caps.conpty());
        const bool requestedSupported = kind == wire::DESKTOP ? caps.capture_available() && (!writeControl || caps.desktop_input_contract() == inputContract) : (kind == wire::VNC ? caps.vnc() : terminalSupported);
        return DeviceSessionCapabilities{true, inputSupported, requestedSupported};
    });
    co_return result.value_or(DeviceSessionCapabilities{});
}

ruvia::task<bool> dispatchSessionOpen(const ruvia::worker_ref& caller, ServerRuntime& local, const std::string& deviceId, wire::Envelope openCommand, const std::vector<std::string>& preemptedSessions) {
    const auto target = resolveWebWorker(deviceId);
    auto result = co_await callOnWebWorker<bool>(caller, local, target, [deviceId, openCommand = std::move(openCommand), preemptedSessions](ServerRuntime& rt) mutable {
        const auto found = rt.controlConnections.find(deviceId);
        if (found == rt.controlConnections.end() || !found->second->ready) {
            return false;
        }
        const auto control = found->second;
        for (const auto& sessionId : preemptedSessions) {
            wire::Envelope close;
            close.set_session_id(sessionId);
            close.mutable_session_close()->set_reason("preempted");
            if (!control->enqueue(std::move(close))) {
                control->close();
                return false;
            }
        }
        return control->enqueue(std::move(openCommand));
    });
    co_return result.value_or(false);
}

void closeDesktopSession(ServerRuntime& local, const std::string& sessionId, DesktopCloseReason reason, DesktopCloseOrigin origin) {
    const auto target = resolveWebWorker(sessionId);
    if (!target.valid() || target.is_current()) {
        if (const auto it = local.desktopSessions.find(sessionId); it != local.desktopSessions.end()) {
            it->second->close(reason, origin);
        }
        return;
    }
    (void)target.post([sessionId, reason, origin](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
        auto& rt = ctx.worker_state<ServerRuntime>();
        if (const auto it = rt.desktopSessions.find(sessionId); it != rt.desktopSessions.end()) {
            it->second->close(reason, origin);
        }
        co_return;
    });
}

void closeTerminalSession(ServerRuntime& local, const std::string& sessionId, TerminalCloseReason reason) {
    const auto target = resolveWebWorker(sessionId);
    if (!target.valid() || target.is_current()) {
        if (const auto it = local.terminalSessions.find(sessionId); it != local.terminalSessions.end()) {
            it->second->close(reason);
        }
        return;
    }
    (void)target.post([sessionId, reason](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
        auto& rt = ctx.worker_state<ServerRuntime>();
        if (const auto it = rt.terminalSessions.find(sessionId); it != rt.terminalSessions.end()) {
            it->second->close(reason);
        }
        co_return;
    });
}
} // namespace ultraterimal::server
