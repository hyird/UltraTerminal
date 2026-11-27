#pragma once
#include <context.h>
#include <task.h>

#include "config/server.runtime.h"

namespace ultraterimal::server {
inline ruvia::task<void> revalidateAccess(ruvia::context& context, std::string userId = {}) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto ended = co_await runtime.sessions.revalidate(context.db(), userId);
    for (const auto& session : ended) {
        closeDesktopSession(runtime, session.id, DesktopCloseReason::kPermissionRevoked);
        sendDeviceSessionClose(runtime, session.deviceId, session.id, "permission_revoked");
    }
    if (!userId.empty()) {
        publishClusterStatus(runtime, "u:" + userId);
    }
}
} // namespace ultraterimal::server
