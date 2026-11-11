#pragma once
#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include "config/server.runtime.h"

namespace ultraterimal::server {
inline ruvia::task<void> revalidateAccess(ruvia::context& context, std::string userId = {}) {
    auto& runtime = (*context.worker_state<std::shared_ptr<ServerRuntime>>());
    const auto ended = ((co_await context.run_blocking([service = runtime.sessions, userId] { return service.revalidate(userId); })).take_value());
    for (const auto& session : ended) {
        const auto desktop = runtime.desktopSessions.find(session.id);
        if (desktop != runtime.desktopSessions.end()) {
            desktop->second->close(DesktopCloseReason::kPermissionRevoked);
        }
        const auto control = runtime.controlConnections.find(session.deviceId);
        if (control != runtime.controlConnections.end()) {
            wire::Envelope command;
            command.set_session_id(session.id);
            command.mutable_session_close()->set_reason("permission_revoked");
            if (!control->second->enqueue(std::move(command))) {
                control->second->close();
            }
        }
    }
    if (!userId.empty()) {
        runtime.status.publish("u:" + userId);
    }
}
} // namespace ultraterimal::server
