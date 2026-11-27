#pragma once
#include <memory>
#include <string>
#include <task.h>

#include "common/database.h"
#include "modules/devices/update.service.h"

namespace ultraterimal::server {
template <typename Context, typename Connection>
ruvia::task<void> serveAgentUpdate(Context& context, Connection& socket) {
    const auto first = co_await socket.read();
    wire::AgentUpdate requested;
    if (!first || first->payload().size() > 1024 || !requested.ParseFromArray(first->payload().data(), static_cast<int>(first->payload().size())) || !protocol::validUpdate(requested)) {
        co_return;
    }
    const auto& deviceId = context.template request_state<DeviceIdentity>().id;
    const auto rows = co_await context.db().query("SELECT platform FROM devices WHERE id=? AND revoked_at IS NULL", deviceId);
    const std::string platform = rows.empty() ? std::string("windows-x64") : dbField<std::string>(rows[0][0]);
    const auto release = ((co_await context.run_blocking([root = context.template worker_state<ServerRuntime>().config.agentReleaseDir, platform] { return agentRelease(root, platform); })).take_value());
    if (!release || release->update.SerializeAsString() != requested.SerializeAsString()) {
        co_return;
    }
    auto file = ((co_await context.run_blocking([path = release->file] { return std::make_shared<std::ifstream>(path, std::ios::binary); })).take_value());
    std::uint64_t offset = 0;
    while (offset < requested.size()) {
        auto data = ((co_await context.run_blocking([file, count = std::min<std::uint64_t>(protocol::kUpdateChunkBytes, requested.size() - offset)] {
                         std::string bytes(static_cast<std::size_t>(count), '\0');
                         file->read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                         if (static_cast<std::size_t>(file->gcount()) != bytes.size()) {
                             throw std::runtime_error("Update package truncated");
                         }
                         return bytes;
                     })).take_value());
        wire::AgentUpdateChunk chunk;
        chunk.set_offset(offset);
        offset += data.size();
        chunk.set_data(std::move(data));
        co_await socket.binary(chunk.SerializeAsString());
    }
    // Client verifies the complete package before acknowledging; keep the last write alive.
    (void)co_await socket.read();
}
} // namespace ultraterimal::server
