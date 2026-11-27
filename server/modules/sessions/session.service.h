#pragma once
#include <db_executor.h>
#include <expected>
#include <optional>
#include <string>
#include <task.h>
#include <utility>
#include <vector>

#include "modules/devices/device.service.h"
#include "modules/identity/identity.service.h"
#include "session.types.h"

namespace ultraterimal::server {
class SessionService {
  public:
    explicit SessionService(std::string key = {}) : key_(std::move(key)) {}
    ruvia::task<std::expected<IssuedSession, SessionFailure>> create(ruvia::db_executor db, SessionRequest request) const;
    ruvia::task<std::optional<ChannelAuthorization>> consumeBrowser(ruvia::db_executor db, UserIdentity user, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const;
    ruvia::task<std::optional<ChannelAuthorization>> consumeAgent(ruvia::db_executor db, DeviceIdentity device, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const;
    ruvia::task<bool> close(ruvia::db_executor db, std::string userId, std::string sessionId, SessionEnd reason) const;
    // Re-checks grants for an established channel; callers must end its live I/O on false.
    ruvia::task<std::vector<SessionInfo>> revalidate(ruvia::db_executor db, std::string userId = {}) const;
    ruvia::task<bool> authorized(ruvia::db_executor db, std::string sessionId) const;
    ruvia::task<bool> authorizeDesktopFileTransfer(ruvia::db_executor db, UserIdentity user, std::string deviceId, std::string sessionId, bool requireWrite) const;
    ruvia::task<void> auditFileTransfer(ruvia::db_executor db, std::string userId, std::string deviceId, std::string sessionId, std::string eventType, bool success, std::string reason = {}) const;
    ruvia::task<std::optional<SessionInfo>> get(ruvia::db_executor db, std::string userId, std::string sessionId) const;
    ruvia::task<bool> closeForDevice(ruvia::db_executor db, DeviceIdentity device, std::string sessionId, SessionEnd reason) const;
    ruvia::task<void> disconnectDevice(ruvia::db_executor db, DeviceIdentity device) const;
    ruvia::task<std::vector<std::string>> revokeDevice(ruvia::db_executor db, std::string deviceId) const;
    // Startup only, while holding the single-gateway database lease.
    ruvia::task<void> recoverAfterRestart(ruvia::db_executor db) const;

  private:
    ruvia::task<std::optional<ChannelAuthorization>> consume(ruvia::db_executor db, std::optional<UserIdentity> user, std::optional<DeviceIdentity> device, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const;
    std::string key_;
};
} // namespace ultraterimal::server
