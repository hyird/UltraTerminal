#pragma once

#include <chrono>
#include <exception>
#include <string_view>

#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>
#include <spdlog/spdlog.h>

#include "config/server.runtime.h"
#include "modules/identity/identity.service.h"
#include "ultraterimal/protocol/message.protocol.h"
#include "ultraterimal/protocol/update.protocol.h"
namespace ultraterimal::server {
namespace {
struct ConnectionLease {
    std::unordered_map<std::string, std::shared_ptr<ControlConnection>>& connections;
    std::string id;
    ~ConnectionLease() {
        connections.at(id)->close();
        connections.erase(id);
    }
};
std::string_view agentErrorName(std::string_view code) {
    if (code == "desktop_failed" || code == "desktop_exit_timeout" || code == "desktop_unavailable" || code == "capability_unavailable") {
        return code;
    }
    return "unknown";
}
template <typename Connection>
ruvia::task<void> writeControl(Connection& socket, ControlConnection& connection, ruvia::task_scope& scope, std::string_view& closeReason) {
    try {
        while (!scope.stop_requested()) {
            auto bytes = co_await connection.receive(scope.get_stop_token());
            if (scope.stop_requested()) {
                break;
            }
            if (!bytes || !connection.ready) {
                if (closeReason == "peer_closed") {
                    closeReason = connection.ready ? "queue_closed" : "server_closed";
                }
                socket.abort();
                break;
            }
            co_await socket.binary(*bytes);
        }
    } catch (const std::exception&) {
        if (closeReason == "peer_closed") {
            closeReason = "write_failed";
        }
        connection.close();
        socket.abort();
    }
}
template <typename Context, typename Connection>
ruvia::task<void> watchControl(Context& context, Connection& socket, ruvia::task_scope& scope, std::chrono::steady_clock::time_point& deadline, std::string_view& closeReason) {
    while (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1), scope.get_stop_token()) == ruvia::timer_sleep_result::elapsed) {
        if (std::chrono::steady_clock::now() >= deadline) {
            if (closeReason == "peer_closed") {
                closeReason = "heartbeat_timeout";
            }
            socket.abort();
            co_return;
        }
    }
}
template <typename Context, typename Connection>
ruvia::task<void> exchangeControl(Context& context, Connection& socket, const DeviceIdentity& device, ControlConnection& connection, std::chrono::steady_clock::time_point& deadline, std::string_view& closeReason) {
    auto message = co_await socket.read();
    if (!message || !message->binary() || message->payload().size() > protocol::kMaximumControlBytes) {
        co_return;
    }
    const auto hello = protocol::decodeMessage(message->payload());
    if (!hello || !hello->has_hello() || hello->hello().device_id() != device.id || !hello->session_id().empty() || !hello->channel_id().empty()) {
        co_return;
    }
    if (!((co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).devices, sessions = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).sessions, device, bytes = hello->hello().capabilities().SerializeAsString()] {
            if (!service.capabilities(device, bytes)) {
                return false;
            }
            sessions.disconnectDevice(device);
            return true;
        })))) {
        co_return;
    }
    std::uint64_t received = hello->sequence();
    wire::Envelope welcome;
    welcome.set_protocol_version(protocol::kVersion);
    welcome.set_sequence(1);
    welcome.mutable_welcome()->set_heartbeat_seconds(15);
    welcome.mutable_welcome()->set_agent_logs(true);
    auto output = protocol::encodeMessage(welcome);
    if (!output) {
        co_return;
    }
    co_await socket.binary(*output);
    connection.capabilities = hello->hello().capabilities();
    connection.agentVersion = hello->hello().agent_version();
    spdlog::info("agent_connected device_id={} version={}", device.id, protocol::updateVersion(connection.agentVersion) ? connection.agentVersion : "unknown");
    if (connection.capabilities.system_restart()) {
        (co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).devices, device, bootTime = connection.capabilities.boot_time_unix_seconds()] { service.confirmRestart(device.id, bootTime); }));
    }
    connection.ready = true;
    (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).status.publish("d:" + device.id);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto logWindow = std::chrono::steady_clock::now();
    unsigned logBatches = 0;
    while ((message = co_await socket.read())) {
        if (!message->binary() || message->payload().size() > protocol::kMaximumControlBytes) {
            closeReason = "invalid_control_message";
            break;
        }
        const auto incoming = protocol::decodeMessage(message->payload());
        if (!incoming || incoming->sequence() <= received || !incoming->channel_id().empty()) {
            closeReason = "invalid_control_message";
            break;
        }
        received = incoming->sequence();
        if (incoming->has_agent_logs()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - logWindow >= std::chrono::seconds(1)) {
                logWindow = now;
                logBatches = 0;
            }
            if (++logBatches > 4) {
                closeReason = "agent_log_rate_exceeded";
                break;
            }
            const auto& batch = incoming->agent_logs();
            for (const auto& entry : batch.entries()) {
                if (entry.event() == wire::AgentLog::UPDATE_FAILED || entry.event() == wire::AgentLog::UPDATE_COMPLETED) {
                    connection.updatePending = false;
                }
                // Identity comes from authenticated transport, never from uploaded log data.
                spdlog::info("agent_log device_id={} runtime_id={} event_sequence={} agent_unix_ms={} event={} session_id={} exit_code={} forced_exit={} fault_stage={} fault_code={} windows_session={} worker_pid={}",
                             device.id, batch.runtime_id(), entry.sequence(), entry.unix_milliseconds(), wire::AgentLog_Event_Name(entry.event()), entry.session_id(), entry.exit_code(), entry.forced_exit(),
                             entry.has_desktop_fault() ? wire::DesktopFault_Stage_Name(entry.desktop_fault().stage()) : "none",
                             entry.has_desktop_fault() ? wire::DesktopFault_Code_Name(entry.desktop_fault().code()) : "none",
                             entry.desktop_fault().windows_session_id(), entry.desktop_fault().process_id());
            }
            if (batch.dropped()) {
                spdlog::warn("agent_log_dropped device_id={} runtime_id={} count={}", device.id, batch.runtime_id(), batch.dropped());
            }
            wire::Envelope response;
            response.mutable_agent_log_ack()->set_sequence(batch.entries().rbegin()->sequence());
            if (!connection.enqueue(std::move(response))) {
                closeReason = "queue_closed";
                break;
            }
            continue;
        }
        if (incoming->has_windows_login_result()) {
            if (!connection.windowsLoginReply(incoming->windows_login_result())) {
                break;
            }
            continue;
        }
        if (incoming->has_windows_restart_result()) {
            const auto result = incoming->windows_restart_result();
            if (!connection.windowsRestartReply(result)) {
                break;
            }
            (co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).devices, device, result] {
                service.applyRestartResult(device, result);
            }));
            continue;
        }
        if (incoming->has_session_error() || incoming->has_session_close()) {
            const bool failed = incoming->has_session_error();
            const auto code = failed ? agentErrorName(incoming->session_error().code()) : std::string_view("agent_closed");
            if (!((co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).sessions, device, sessionId = incoming->session_id(), reason = failed ? SessionEnd::kFailure : SessionEnd::kAgentClosed] { return service.closeForDevice(device, sessionId, reason); })))) {
                closeReason = "invalid_session_event";
                break;
            }
            spdlog::info("desktop_agent_event session_id={} device_id={} outcome={} code={}", incoming->session_id(), device.id, failed ? "failed" : "closed", code);
            continue;
        }
        if (!incoming->has_heartbeat() || !incoming->session_id().empty()) {
            closeReason = "invalid_control_message";
            break;
        }
        if (!((co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).devices, device] { return service.heartbeat(device); })))) {
            closeReason = "heartbeat_rejected";
            break;
        }
        wire::Envelope response;
        response.mutable_heartbeat()->set_monotonic_milliseconds(incoming->heartbeat().monotonic_milliseconds());
        if (!connection.enqueue(std::move(response))) {
            closeReason = "queue_closed";
            break;
        }
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
}
} // namespace
template <typename Context, typename Connection>
ruvia::task<void> runControlConnection(Context& context, Connection& socket) {
    const auto device = context.template request_state<DeviceIdentity>();
    auto& connections = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).controlConnections;
    const auto connection = std::make_shared<ControlConnection>(context.worker());
    if (!connections.emplace(device.id, connection).second) {
        socket.abort();
        co_return;
    }
    ConnectionLease lease{connections, device.id};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto started = std::chrono::steady_clock::now();
    std::string_view closeReason = "peer_closed";
    {
        const auto scopeWorker = context.worker();
        ruvia::task_scope scope(scopeWorker);
        bool scopeJoined = false;
        std::exception_ptr scopeFailure;
        try {
            scope.spawn(watchControl(context, socket, scope, deadline, closeReason));
            scope.spawn(writeControl(socket, *connection, scope, closeReason));
            try {
                co_await exchangeControl(context, socket, device, *connection, deadline, closeReason);
            } catch (const std::exception&) {
                if (closeReason == "peer_closed") {
                    closeReason = "read_or_storage_failed";
                }
            }
            const auto dispatchedRestarts = connection->restartOperations();
            connection->close();
            for (const auto& operationId : dispatchedRestarts) {
                wire::WindowsRestartResult unknown;
                unknown.set_operation_id(operationId);
                unknown.set_status(wire::WindowsRestartResult::UNKNOWN);
                try {
                    (co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).devices, device, unknown] {
                        service.applyRestartResult(device, unknown);
                    }));
                } catch (const std::exception&) { /* Pending operations expire conservatively if storage is unavailable. */
                }
            }
            (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).status.publish("d:" + device.id);
            scope.request_stop();
            scopeJoined = true;
            co_await scope.join();
            spdlog::info("agent_control_end device_id={} reason={} transport={} duration_ms={}", device.id, closeReason, context.transport_close_cause(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
            try {
                (co_await context.run_blocking([service = (*context.template worker_state<std::shared_ptr<ServerRuntime>>()).sessions, device] { service.disconnectDevice(device); }));
            } catch (const std::exception&) { /* Recovery on the next HELLO or gateway restart closes stale database sessions. */
            }

        } catch (...) {
            scopeFailure = std::current_exception();
            connection->close();
            socket.abort();
        }
        if (!scopeJoined) {
            scope.request_stop();
            try {
                co_await scope.join();
            } catch (...) {
                if (!scopeFailure) {
                    scopeFailure = std::current_exception();
                }
            }
        }
        if (scopeFailure) {
            std::rethrow_exception(scopeFailure);
        }
    }
}
} // namespace ultraterimal::server
