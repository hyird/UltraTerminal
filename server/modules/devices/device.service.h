#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modules/database/database.config.h"
#include "ultraterimal/protocol/message.protocol.h"
namespace ultraterimal::server {
struct DeviceRegistration {
    std::string deviceId;
    std::string credential;
};
struct DeviceIdentity {
    std::string id;
    std::string credentialHash;
};
struct DeviceInfo {
    std::string id;
    std::string name;
    std::optional<std::string> lastSeen;
    bool canManage = false;
    bool enrolled = true;
    bool desktop = false;
    bool writeControl = false;
    bool requireApproval = false;
    bool manageCredentials = false;
    std::string ownerUsername = {};
};
struct DeviceGrant {
    std::string userId, username;
    bool desktop = true;
    bool writeControl = false;
    bool systemRestart = false;
    bool manageCredentials = false;
    bool requireApproval = false;
};
struct RestartOperation {
    std::string id;
    std::string deviceId;
    std::string status;
    std::uint64_t expiresAtUnixSeconds = 0;
    std::uint64_t bootTimeUnixSeconds = 0;
    bool newlyCreated = false;
};
class DeviceService {
  public:
    explicit DeviceService(DatabaseConfig database) : database_(std::move(database)) {}
    DeviceInfo createDevice(std::string_view userId, std::string_view name) const;
    std::optional<std::string> createEnrollment(std::string_view userId, std::string_view deviceId) const;
    std::optional<std::string> enrollmentDevice(std::string_view code) const;
    std::optional<DeviceRegistration> registerDevice(std::string_view code, std::string_view name) const;
    std::optional<DeviceIdentity> authenticate(std::string_view credential) const;
    bool heartbeat(const DeviceIdentity& device) const;
    bool capabilities(const DeviceIdentity& device, std::string_view protobuf) const;
    std::vector<DeviceInfo> list(std::string_view userId, bool administration = false) const;
    bool rename(std::string_view userId, std::string_view deviceId, std::string_view name) const;
    bool remove(std::string_view userId, std::string_view deviceId) const;
    std::optional<std::vector<DeviceGrant>> grants(std::string_view actorId, std::string_view deviceId) const;
    bool setGrant(std::string_view actorId, std::string_view deviceId, const DeviceGrant& grant, bool remove) const;
    std::optional<DeviceInfo> find(std::string_view userId, std::string_view deviceId, bool administration = false) const;
    bool canConfigureWindowsLogin(std::string_view userId, std::string_view deviceId) const;
    std::optional<bool> systemRestartPermission(std::string_view userId, std::string_view deviceId) const;
    bool restartDispatchAllowed(std::string_view userId, std::string_view deviceId) const;
    std::optional<RestartOperation> restartByIdempotencyKey(std::string_view userId, std::string_view deviceId, std::string_view idempotencyKey) const;
    std::optional<RestartOperation> createRestart(std::string_view userId, std::string_view deviceId, std::string_view idempotencyKey, std::uint64_t bootTimeUnixSeconds) const;
    std::optional<RestartOperation> restart(std::string_view userId, std::string_view operationId) const;
    bool applyRestartResult(const DeviceIdentity& device, const wire::WindowsRestartResult& result) const;
    void updateRestart(std::string_view operationId, std::string_view status, std::optional<std::uint32_t> win32Error = std::nullopt) const;
    void confirmRestart(std::string_view deviceId, std::uint64_t bootTimeUnixSeconds) const;
    void recoverRestarts() const;
    void auditWindowsLogin(std::string_view userId, std::string_view deviceId, std::string_view action, bool success) const;

  private:
    DatabaseConfig database_;
};
} // namespace ultraterimal::server
