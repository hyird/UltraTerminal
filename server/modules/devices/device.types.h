#pragma once
#include <cstdint>
#include <model.h>
#include <optional>
#include <string>

#include "common/http.h"

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
    std::string verificationMethod = "direct";
    bool manageCredentials = false;
    std::string ownerUsername = {};
    std::optional<std::string> agentVersion = {};
    std::string platform = "windows-x64";
};
struct DeviceGrant {
    std::string userId, username;
    bool desktop = true;
    bool writeControl = false;
    bool systemRestart = false;
    bool manageCredentials = false;
    std::string verificationMethod = "direct";
};
struct RestartOperation {
    std::string id;
    std::string deviceId;
    std::string status;
    std::uint64_t expiresAtUnixSeconds = 0;
    std::uint64_t bootTimeUnixSeconds = 0;
    bool newlyCreated = false;
};

RUVIA_MODEL(InstallManifestRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(128, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(credential, ruvia::string, RUVIA_MAX_BYTES(128, "credential too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(platform, ruvia::string, RUVIA_MAX_BYTES(32, "platform too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(InstallPackageRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(128, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(credential, ruvia::string, RUVIA_MAX_BYTES(128, "credential too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(sha256, ruvia::string, RUVIA_MAX_BYTES(64, "sha256 too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(platform, ruvia::string, RUVIA_MAX_BYTES(32, "platform too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceEnrollmentRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(platform, ruvia::string, RUVIA_NOT_EMPTY("platform required"), RUVIA_MAX_BYTES(32, "platform too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceCreateRequest,
            RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_NOT_EMPTY("name required"), RUVIA_MAX_BYTES(128, "name too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(platform, ruvia::string, RUVIA_MAX_BYTES(32, "platform too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceRenameRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_NOT_EMPTY("name required"), RUVIA_MAX_BYTES(128, "name too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceTransferOwnerRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(owner_id, ruvia::string, RUVIA_NOT_EMPTY("owner_id required"), RUVIA_MAX_BYTES(128, "owner_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(current_password, ruvia::string, RUVIA_MAX_BYTES(1024, "current_password too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(code, ruvia::string, RUVIA_MAX_BYTES(32, "code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(telegram_proof, ruvia::string, RUVIA_MAX_BYTES(32, "telegram_proof too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceIdRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(WindowsLoginApiRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(action, ruvia::string, RUVIA_NOT_EMPTY("action required"), RUVIA_MAX_BYTES(8, "action too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(username, ruvia::string, RUVIA_MAX_BYTES(512, "username too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(domain, ruvia::string, RUVIA_MAX_BYTES(256, "domain too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(password, ruvia::string, RUVIA_MAX_BYTES(1024, "password too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(RestartPermissionQuery,
            RUVIA_REQUIRED_FIELD(deviceId, ruvia::string, RUVIA_NOT_EMPTY("deviceId required"), RUVIA_MAX_BYTES(128, "deviceId too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceRestartRequest,
            RUVIA_REQUIRED_FIELD(deviceId, ruvia::string, RUVIA_NOT_EMPTY("deviceId required"), RUVIA_MAX_BYTES(128, "deviceId too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(idempotencyKey, ruvia::string, RUVIA_NOT_EMPTY("idempotencyKey required"), RUVIA_MAX_BYTES(128, "idempotencyKey too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(DeviceGrantRequest,
            RUVIA_REQUIRED_FIELD(username, ruvia::string, RUVIA_NOT_EMPTY("username required"), RUVIA_MAX_BYTES(64, "username too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(connect, bool),
            RUVIA_REQUIRED_FIELD(verification_method, ruvia::string, RUVIA_NOT_EMPTY("verification_method required"), RUVIA_MAX_BYTES(16, "verification_method too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(remove, bool));
RUVIA_MODEL(DeviceUpdateResponse,
            RUVIA_REQUIRED_FIELD(status, ruvia::string),
            RUVIA_REQUIRED_FIELD(version, ruvia::string));
RUVIA_MODEL(InstallManifestResponse,
            RUVIA_REQUIRED_FIELD(version, ruvia::string),
            RUVIA_REQUIRED_FIELD(file, ruvia::string),
            RUVIA_REQUIRED_FIELD(sha256, ruvia::string),
            RUVIA_REQUIRED_FIELD(size, std::uint64_t),
            RUVIA_REQUIRED_FIELD(native_host, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_address, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_port, std::uint16_t));
RUVIA_MODEL(DeviceRenameResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string));
RUVIA_MODEL(DeviceTransferOwnerResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(owner_id, ruvia::string));
RUVIA_MODEL(DeviceDeleteResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(deleted, bool));
RUVIA_MODEL(WindowsLoginResponse,
            RUVIA_REQUIRED_FIELD(saved, bool),
            RUVIA_REQUIRED_FIELD(username, ruvia::string),
            RUVIA_REQUIRED_FIELD(domain, ruvia::string));
RUVIA_MODEL(RestartPermissionResponse,
            RUVIA_REQUIRED_FIELD(allowed, bool));
RUVIA_MODEL(RestartOperationResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(DeviceEnrollmentResponse,
            RUVIA_REQUIRED_FIELD(code, ruvia::string),
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(expires_in, std::int32_t),
            RUVIA_REQUIRED_FIELD(web_origin, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_host, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_address, ruvia::string),
            RUVIA_REQUIRED_FIELD(native_port, std::uint16_t));
RUVIA_MODEL(DeviceCreateResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(platform, ruvia::string),
            RUVIA_REQUIRED_FIELD(enrolled, bool),
            RUVIA_REQUIRED_FIELD(can_manage, bool),
            RUVIA_REQUIRED_FIELD(desktop, bool),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_REQUIRED_FIELD(verification_method, ruvia::string),
            RUVIA_REQUIRED_FIELD(manage_credentials, bool),
            RUVIA_REQUIRED_FIELD(online, bool),
            RUVIA_REQUIRED_FIELD(capture_available, bool),
            RUVIA_REQUIRED_FIELD(terminal_available, bool),
            RUVIA_REQUIRED_FIELD(windows_login_available, bool),
            RUVIA_OPTIONAL_FIELD(last_seen, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL));
RUVIA_MODEL(DeviceListItem,
            RUVIA_OPTIONAL_FIELD(agent_version, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(name, ruvia::string),
            RUVIA_REQUIRED_FIELD(platform, ruvia::string),
            RUVIA_REQUIRED_FIELD(enrolled, bool),
            RUVIA_REQUIRED_FIELD(can_manage, bool),
            RUVIA_REQUIRED_FIELD(desktop, bool),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_REQUIRED_FIELD(verification_method, ruvia::string),
            RUVIA_REQUIRED_FIELD(manage_credentials, bool),
            RUVIA_REQUIRED_FIELD(online, bool),
            RUVIA_REQUIRED_FIELD(capture_available, bool),
            RUVIA_REQUIRED_FIELD(terminal_available, bool),
            RUVIA_REQUIRED_FIELD(windows_login_available, bool),
            RUVIA_OPTIONAL_FIELD(last_seen, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL));
RUVIA_MODEL(DeviceGrantItem,
            RUVIA_REQUIRED_FIELD(user_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(username, ruvia::string),
            RUVIA_REQUIRED_FIELD(connect, bool),
            RUVIA_REQUIRED_FIELD(verification_method, ruvia::string));
} // namespace ultraterimal::server
