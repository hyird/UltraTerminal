#pragma once
#include <cstdint>
#include <db_executor.h>
#include <optional>
#include <string>
#include <task.h>
#include <vector>

#include "device.types.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
class DeviceService {
  public:
    ruvia::task<DeviceInfo> createDevice(ruvia::db_executor db, std::string userId, std::string name) const;
    ruvia::task<std::optional<std::string>> createEnrollment(ruvia::db_executor db, std::string userId, std::string deviceId) const;
    ruvia::task<std::optional<std::string>> enrollmentDevice(ruvia::db_executor db, std::string code) const;
    ruvia::task<std::optional<DeviceRegistration>> registerDevice(ruvia::db_executor db, std::string code, std::string name) const;
    ruvia::task<std::optional<DeviceIdentity>> authenticate(ruvia::db_executor db, std::string credential) const;
    ruvia::task<bool> heartbeat(ruvia::db_executor db, DeviceIdentity device) const;
    ruvia::task<bool> capabilities(ruvia::db_executor db, DeviceIdentity device, std::string protobuf, std::string agentVersion = {}) const;
    ruvia::task<std::vector<DeviceInfo>> list(ruvia::db_executor db, std::string userId, bool administration = false) const;
    ruvia::task<bool> rename(ruvia::db_executor db, std::string userId, std::string deviceId, std::string name) const;
    ruvia::task<bool> transferOwner(ruvia::db_executor db, std::string actorId, std::string deviceId, std::string targetUserId) const;
    ruvia::task<bool> remove(ruvia::db_executor db, std::string userId, std::string deviceId) const;
    ruvia::task<std::optional<std::vector<DeviceGrant>>> grants(ruvia::db_executor db, std::string actorId, std::string deviceId) const;
    ruvia::task<bool> setGrant(ruvia::db_executor db, std::string actorId, std::string deviceId, DeviceGrant grant, bool remove) const;
    ruvia::task<std::optional<DeviceInfo>> find(ruvia::db_executor db, std::string userId, std::string deviceId, bool administration = false) const;
    ruvia::task<bool> canConfigureWindowsLogin(ruvia::db_executor db, std::string userId, std::string deviceId) const;
    ruvia::task<std::optional<bool>> systemRestartPermission(ruvia::db_executor db, std::string userId, std::string deviceId) const;
    ruvia::task<bool> restartDispatchAllowed(ruvia::db_executor db, std::string userId, std::string deviceId) const;
    ruvia::task<std::optional<RestartOperation>> restartByIdempotencyKey(ruvia::db_executor db, std::string userId, std::string deviceId, std::string idempotencyKey) const;
    ruvia::task<std::optional<RestartOperation>> createRestart(ruvia::db_executor db, std::string userId, std::string deviceId, std::string idempotencyKey, std::uint64_t bootTimeUnixSeconds) const;
    ruvia::task<std::optional<RestartOperation>> restart(ruvia::db_executor db, std::string userId, std::string operationId) const;
    ruvia::task<bool> applyRestartResult(ruvia::db_executor db, DeviceIdentity device, wire::WindowsRestartResult result) const;
    ruvia::task<void> updateRestart(ruvia::db_executor db, std::string operationId, std::string status, std::optional<std::uint32_t> win32Error = std::nullopt) const;
    ruvia::task<void> confirmRestart(ruvia::db_executor db, std::string deviceId, std::uint64_t bootTimeUnixSeconds) const;
    ruvia::task<void> recoverRestarts(ruvia::db_executor db) const;
    ruvia::task<void> auditWindowsLogin(ruvia::db_executor db, std::string userId, std::string deviceId, std::string action, bool success) const;
};
} // namespace ultraterimal::server
