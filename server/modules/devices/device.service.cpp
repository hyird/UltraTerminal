#include "device.service.h"

#include <stdexcept>

#include "modules/database/database.service.h"
#include "modules/identity/identity.service.h"
#include "ultraterimal/protocol/update.protocol.h"
namespace ultraterimal::server {
DeviceInfo DeviceService::createDevice(std::string_view userId, std::string_view name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Invalid device name");
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", userId).empty()) {
        throw std::runtime_error("User unavailable");
    }
    if (tx.exec("SELECT count(*) FROM device_grants g JOIN devices d ON d.id=g.device_id WHERE g.user_id=? AND d.revoked_at IS NULL", userId)[0][0].as<int>() >= 1000) {
        throw std::runtime_error("Device limit reached");
    }
    DeviceInfo result{"d-" + randomToken(), std::string(name), std::nullopt, true, false, true, true, "direct", true};
    tx.exec("INSERT INTO devices(id,name,credential_hash,enrolled,owner_id) VALUES(?,?,?,false,?)", result.id, result.name, tokenHash(randomToken()), userId);
    tx.exec("INSERT INTO device_grants(device_id,user_id,system_restart,manage_credentials) VALUES(?,?,true,true)", result.id, userId);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.create',true)", userId, result.id);
    tx.commit();
    return result;
}
std::optional<std::string> DeviceService::createEnrollment(std::string_view userId, std::string_view deviceId) const {
    const auto code = randomToken();
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto device = tx.exec("SELECT d.enrolled FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND d.revoked_at IS NULL AND NOT u.disabled AND d.owner_id=u.id FOR UPDATE", userId, deviceId);
    if (device.empty()) {
        return std::nullopt;
    }
    if (device[0][0].as<bool>()) {
        return std::string{};
    }
    tx.exec("DELETE FROM enrollment_codes WHERE expires_at<now(6)-INTERVAL 1 DAY");
    if (tx.exec("SELECT count(*) FROM enrollment_codes WHERE user_id=? AND consumed_at IS NULL AND expires_at>now(6)", userId)[0][0].as<int>() >= 20) {
        throw std::runtime_error("Enrollment limit reached");
    }
    tx.exec("UPDATE enrollment_codes SET expires_at=now(6) WHERE device_id=? AND consumed_at IS NULL", deviceId);
    tx.exec("INSERT INTO enrollment_codes(code_hash,user_id,device_id,expires_at) VALUES(?,?,?,now(6)+INTERVAL 10 MINUTE)", tokenHash(code), userId, deviceId);
    tx.commit();
    return code;
}
std::optional<std::string> DeviceService::enrollmentDevice(std::string_view code) const {
    if (!validToken(code)) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT d.id FROM enrollment_codes e JOIN devices d ON d.id=e.device_id JOIN users u ON u.id=e.user_id WHERE e.code_hash=? AND e.consumed_at IS NULL AND e.expires_at>now(6) AND NOT u.disabled AND NOT d.enrolled AND d.revoked_at IS NULL", tokenHash(code));
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows[0][0].as<std::string>();
}
std::optional<DeviceRegistration> DeviceService::registerDevice(std::string_view code, std::string_view name) const {
    if (!validToken(code) || name.empty() || name.size() > 128 || name.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT user_id,device_id FROM enrollment_codes WHERE code_hash=? AND device_id IS NOT NULL AND consumed_at IS NULL AND expires_at>now(6) AND user_id IN (SELECT id FROM users WHERE NOT disabled) FOR UPDATE", tokenHash(code));
    tx.exec("UPDATE enrollment_codes SET consumed_at=now(6) WHERE code_hash=? AND device_id IS NOT NULL AND consumed_at IS NULL AND expires_at>now(6) AND user_id IN (SELECT id FROM users WHERE NOT disabled)", tokenHash(code));
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto userId = rows[0][0].as<std::string>();
    DeviceRegistration result{rows[0][1].as<std::string>(), randomToken()};
    if (tx.exec("UPDATE devices SET credential_hash=?,enrolled=true WHERE id=? AND NOT enrolled AND revoked_at IS NULL", tokenHash(result.credential), result.deviceId).affectedRows == 0) {
        return std::nullopt;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.register',true)", userId, result.deviceId);
    tx.commit();
    return result;
}
std::optional<DeviceIdentity> DeviceService::authenticate(std::string_view credential) const {
    if (!validToken(credential)) {
        return std::nullopt;
    }
    const auto hash = tokenHash(credential);
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT id FROM devices WHERE credential_hash=? AND revoked_at IS NULL", hash);
    if (rows.empty()) {
        return std::nullopt;
    }
    return DeviceIdentity{rows[0][0].as<std::string>(), hash};
}
bool DeviceService::heartbeat(const DeviceIdentity& device) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT id FROM devices WHERE id=? AND credential_hash=? AND revoked_at IS NULL FOR UPDATE", device.id, device.credentialHash);
    tx.exec("UPDATE devices SET last_seen_at=now(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", device.id, device.credentialHash);
    tx.commit();
    return !rows.empty();
}
bool DeviceService::capabilities(const DeviceIdentity& device, std::string_view protobuf, std::string_view agentVersion) const {
    if (protobuf.size() > 65536) {
        return false;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=? AND credential_hash=? AND revoked_at IS NULL FOR UPDATE", device.id, device.credentialHash).empty()) {
        return false;
    }
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(protobuf.data()), protobuf.size());
    tx.exec("INSERT INTO device_capabilities(device_id,protocol_version,protobuf) VALUES(?,1,?) ON DUPLICATE KEY UPDATE protocol_version=1,protobuf=VALUES(protobuf),updated_at=now(6)", device.id, bytes);
    if (!agentVersion.empty() && agentVersion.size() <= 64 && protocol::updateVersion(agentVersion)) {
        tx.exec("UPDATE devices SET agent_version=?,last_seen_at=now(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", agentVersion, device.id, device.credentialHash);
    } else {
        tx.exec("UPDATE devices SET last_seen_at=now(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", device.id, device.credentialHash);
    }
    tx.commit();
    return true;
}
std::vector<DeviceInfo> DeviceService::list(std::string_view userId, bool administration) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT d.id,d.name,DATE_FORMAT(d.last_seen_at,'%Y-%m-%dT%H:%i:%s.%fZ'),d.owner_id=u.id,d.enrolled,COALESCE(g.desktop,false),COALESCE(g.write_control,false),COALESCE(g.verification_method,'direct'),d.owner_id=u.id,owner.username,d.agent_version FROM devices d JOIN users owner ON owner.id=d.owner_id JOIN users u ON u.id=? LEFT JOIN device_grants g ON g.device_id=d.id AND g.user_id=u.id WHERE ((? AND u.role='admin') OR (NOT ? AND g.user_id=u.id)) AND NOT u.disabled AND d.revoked_at IS NULL ORDER BY d.created_at LIMIT 1000", userId, administration, administration);
    std::vector<DeviceInfo> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::optional<std::string>>(), row[3].as<bool>(), row[4].as<bool>(), row[5].as<bool>(), row[6].as<bool>(), row[7].as<std::string>(), row[8].as<bool>(), row[9].as<std::string>(), row[10].as<std::optional<std::string>>()});
    }
    return result;
}
bool DeviceService::rename(std::string_view userId, std::string_view deviceId, std::string_view name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT d.id FROM devices d WHERE d.id=? AND d.revoked_at IS NULL AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled) FOR UPDATE", deviceId, userId, userId);
    tx.exec("UPDATE devices d SET name=? WHERE d.id=? AND d.revoked_at IS NULL AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled)", name, deviceId, userId, userId);
    if (rows.empty()) {
        return false;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.rename',true)", userId, deviceId);
    tx.commit();
    return true;
}
bool DeviceService::transferOwner(std::string_view actorId, std::string_view deviceId, std::string_view targetUserId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto device = tx.exec("SELECT owner_id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId);
    if (device.empty()) {
        return false;
    }
    if (tx.exec("SELECT id FROM users WHERE id=? AND role='admin' AND NOT disabled FOR UPDATE", actorId).empty()) {
        return false;
    }
    const auto oldOwnerId = device[0][0].as<std::string>();
    if (oldOwnerId == targetUserId || tx.exec("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", targetUserId).empty()) {
        return false;
    }
    tx.exec("UPDATE devices SET owner_id=? WHERE id=?", targetUserId, deviceId);
    tx.exec("DELETE FROM device_grants WHERE device_id=? AND user_id=?", deviceId, oldOwnerId);
    tx.exec("INSERT INTO device_grants(device_id,user_id,desktop,vnc,ssh,write_control,system_restart,manage_credentials) VALUES(?,?,true,false,false,true,true,true) ON DUPLICATE KEY UPDATE desktop=true,vnc=false,ssh=false,write_control=true,system_restart=true,manage_credentials=true,revision=NEXT VALUE FOR grant_revision_sequence", deviceId, targetUserId);
    tx.exec("UPDATE enrollment_codes SET expires_at=now(6) WHERE device_id=? AND consumed_at IS NULL", deviceId);
    tx.exec("UPDATE connection_verifications SET state='cancelled' WHERE device_id=? AND user_id IN (?,?) AND state IN ('pending','approved')", deviceId, oldOwnerId, targetUserId);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.owner.transfer_from',true,?)", actorId, deviceId, oldOwnerId);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.owner.transfer_to',true,?)", actorId, deviceId, targetUserId);
    tx.commit();
    return true;
}
bool DeviceService::remove(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT d.id FROM devices d WHERE d.id=? AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled) FOR UPDATE", deviceId, userId, userId);
    tx.exec("UPDATE devices d SET revoked_at=COALESCE(revoked_at,now(6)) WHERE d.id=? AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled)", deviceId, userId, userId);
    if (rows.empty()) {
        return false;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.delete',true)", userId, deviceId);
    tx.commit();
    return true;
}
std::optional<bool> DeviceService::systemRestartPermission(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT (g.desktop AND g.write_control AND d.owner_id=g.user_id) FROM device_grants g JOIN devices d ON d.id=g.device_id JOIN users u ON u.id=g.user_id WHERE g.device_id=? AND g.user_id=? AND d.revoked_at IS NULL AND NOT u.disabled", deviceId, userId);
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows[0][0].as<bool>();
}
bool DeviceService::restartDispatchAllowed(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    return !tx.exec("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE d.id=? AND g.user_id=? AND (g.desktop AND g.write_control AND d.owner_id=g.user_id) AND d.revoked_at IS NULL AND NOT u.disabled", deviceId, userId).empty();
}
std::optional<RestartOperation> DeviceService::restartByIdempotencyKey(std::string_view userId, std::string_view deviceId, std::string_view idempotencyKey) const {
    if (idempotencyKey.empty() || idempotencyKey.size() > 128 || idempotencyKey.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT operation_id,device_id,status,FLOOR(UNIX_TIMESTAMP(expires_at)),COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=? AND idempotency_key=? AND device_id=?", userId, idempotencyKey, deviceId);
    if (rows.empty()) {
        return std::nullopt;
    }
    return RestartOperation{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::string>(), static_cast<std::uint64_t>(rows[0][3].as<std::int64_t>()), static_cast<std::uint64_t>(rows[0][4].as<std::int64_t>())};
}
std::optional<RestartOperation> DeviceService::createRestart(std::string_view userId, std::string_view deviceId, std::string_view idempotencyKey, std::uint64_t bootTimeUnixSeconds) const {
    if (idempotencyKey.empty() || idempotencyKey.size() > 128 || idempotencyKey.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto duplicate = tx.exec("SELECT operation_id,device_id,status,FLOOR(UNIX_TIMESTAMP(expires_at)),COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=? AND idempotency_key=?", userId, idempotencyKey);
    if (!duplicate.empty()) {
        if (duplicate[0][1].as<std::string>() != deviceId) {
            tx.commit();
            return std::nullopt;
        }
        RestartOperation result{duplicate[0][0].as<std::string>(), duplicate[0][1].as<std::string>(), duplicate[0][2].as<std::string>(), static_cast<std::uint64_t>(duplicate[0][3].as<std::int64_t>()), static_cast<std::uint64_t>(duplicate[0][4].as<std::int64_t>())};
        tx.commit();
        return result;
    }
    const auto device = tx.exec("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId);
    if (device.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT id,NULL,'device.system_restart.request',false,'denied' FROM users WHERE id=?", userId);
        tx.commit();
        return std::nullopt;
    }
    const auto allowed = tx.exec("SELECT 1 FROM device_grants g JOIN devices d ON d.id=g.device_id JOIN users u ON u.id=g.user_id WHERE g.device_id=? AND g.user_id=? AND (g.desktop AND g.write_control AND d.owner_id=g.user_id) AND NOT u.disabled FOR UPDATE", deviceId, userId);
    if (allowed.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.request',false,'permission_denied')", userId, deviceId);
        tx.commit();
        return std::nullopt;
    }
    RestartOperation result{randomToken(), std::string(deviceId), "PENDING", 0, bootTimeUnixSeconds, true};
    const auto rows = tx.exec("INSERT INTO restart_operations(operation_id,idempotency_key,user_id,device_id,status,expires_at,boot_time_unix_seconds) VALUES(?,?,?,?,'PENDING',now(6)+INTERVAL 30 SECOND,?) RETURNING FLOOR(UNIX_TIMESTAMP(expires_at))", result.id, idempotencyKey, userId, deviceId, bootTimeUnixSeconds);
    result.expiresAtUnixSeconds = static_cast<std::uint64_t>(rows[0][0].as<std::int64_t>());
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.system_restart.request',true)", userId, deviceId);
    tx.commit();
    return result;
}
std::optional<RestartOperation> DeviceService::restart(std::string_view userId, std::string_view operationId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto expired = tx.exec("SELECT user_id,device_id FROM restart_operations WHERE operation_id=? AND user_id=? AND status='PENDING' AND expires_at<=now(6) FOR UPDATE", operationId, userId);
    tx.exec("UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=now(6) WHERE operation_id=? AND user_id=? AND status='PENDING' AND expires_at<=now(6)", operationId, userId);
    if (!expired.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',false,'pending_expired')", expired[0][0].as<std::string>(), expired[0][1].as<std::string>());
    }
    const auto rows = tx.exec("SELECT r.operation_id,r.device_id,r.status,FLOOR(UNIX_TIMESTAMP(r.expires_at)),COALESCE(r.boot_time_unix_seconds,0) FROM restart_operations r JOIN users u ON u.id=r.user_id WHERE r.operation_id=? AND r.user_id=? AND NOT u.disabled", operationId, userId);
    if (rows.empty()) {
        tx.commit();
        return std::nullopt;
    }
    RestartOperation result{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::string>(), static_cast<std::uint64_t>(rows[0][3].as<std::int64_t>()), static_cast<std::uint64_t>(rows[0][4].as<std::int64_t>())};
    tx.commit();
    return result;
}
bool DeviceService::applyRestartResult(const DeviceIdentity& device, const wire::WindowsRestartResult& result) const {
    if (result.operation_id().empty() || !wire::WindowsRestartResult_Status_IsValid(result.status()) ||
        result.status() == wire::WindowsRestartResult::UNSPECIFIED) {
        return false;
    }
    const auto status = result.status() == wire::WindowsRestartResult::ACCEPTED ? "ACCEPTED" : result.status() == wire::WindowsRestartResult::FAILED ? "FAILED"
                                                                                                                                                     : "UNKNOWN";
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT r.user_id,r.device_id FROM restart_operations r JOIN devices d ON d.id=r.device_id WHERE r.operation_id=? AND r.device_id=? AND d.credential_hash=? AND d.revoked_at IS NULL AND r.status IN ('PENDING','UNKNOWN') AND NOT (r.status='UNKNOWN' AND ?='UNKNOWN') FOR UPDATE", result.operation_id(), device.id, device.credentialHash, status);
    tx.exec("UPDATE restart_operations r JOIN devices d ON d.id=r.device_id SET r.status=?,r.result_status=?,r.win32_error=?,r.updated_at=now(6) WHERE r.operation_id=? AND r.device_id=? AND d.credential_hash=? AND d.revoked_at IS NULL AND r.status IN ('PENDING','UNKNOWN') AND NOT (r.status='UNKNOWN' AND ?='UNKNOWN')", status, status, result.status() == wire::WindowsRestartResult::FAILED ? std::optional<std::uint32_t>(result.win32_error()) : std::nullopt, result.operation_id(), device.id, device.credentialHash, status);
    if (!rows.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',?,?)", rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), status == std::string_view("ACCEPTED"), status);
    }
    tx.commit();
    return !rows.empty();
}
void DeviceService::updateRestart(std::string_view operationId, std::string_view status, std::optional<std::uint32_t> win32Error) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT user_id,device_id FROM restart_operations WHERE operation_id=? AND status='PENDING' AND ?='FAILED' FOR UPDATE", operationId, status);
    tx.exec("UPDATE restart_operations SET status=?,result_status=?,win32_error=?,updated_at=now(6) WHERE operation_id=? AND status='PENDING' AND ?='FAILED'", status, status == "PENDING" ? std::optional<std::string>{} : std::optional<std::string>{status}, win32Error, operationId, status);
    if (!rows.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',?,?)", rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), status == "ACCEPTED", status);
    }
    tx.commit();
}
void DeviceService::confirmRestart(std::string_view deviceId, std::uint64_t bootTimeUnixSeconds) const {
    // Wall-clock/uptime deltas cannot prove a Windows reboot: clock correction and
    // Agent-only restarts produce indistinguishable observations. Keep operations
    // ACCEPTED until the control protocol carries reliable boot evidence.
    (void)deviceId;
    (void)bootTimeUnixSeconds;
}
void DeviceService::recoverRestarts() const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT user_id,device_id FROM restart_operations WHERE status='PENDING' FOR UPDATE");
    tx.exec("UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=now(6) WHERE status='PENDING'");
    for (const auto& row : rows) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',false,'gateway_restarted')", row[0].as<std::string>(), row[1].as<std::string>());
    }
    tx.commit();
}
bool DeviceService::canConfigureWindowsLogin(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    return !tx.exec("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE g.user_id=? AND d.id=? AND NOT u.disabled AND d.revoked_at IS NULL AND d.owner_id=u.id", userId, deviceId).empty();
}
void DeviceService::auditWindowsLogin(std::string_view userId, std::string_view deviceId, std::string_view action, bool success) const {
    if (action != "save" && action != "clear") {
        return;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,?,?)", userId, deviceId, std::string("device.windows_login.") + std::string(action), success);
    tx.commit();
}
std::optional<DeviceInfo> DeviceService::find(std::string_view userId, std::string_view deviceId, bool administration) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT d.id,d.name,DATE_FORMAT(d.last_seen_at,'%Y-%m-%dT%H:%i:%s.%fZ'),d.owner_id=u.id,d.enrolled,COALESCE(g.desktop,false),COALESCE(g.write_control,false),COALESCE(g.verification_method,'direct'),d.owner_id=u.id,owner.username,d.agent_version FROM devices d JOIN users owner ON owner.id=d.owner_id JOIN users u ON u.id=? LEFT JOIN device_grants g ON g.device_id=d.id AND g.user_id=u.id WHERE ((? AND u.role='admin') OR (NOT ? AND g.user_id=u.id)) AND d.id=? AND NOT u.disabled AND d.revoked_at IS NULL", userId, administration, administration, deviceId);
    if (rows.empty()) {
        return std::nullopt;
    }
    return DeviceInfo{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::optional<std::string>>(), rows[0][3].as<bool>(), rows[0][4].as<bool>(), rows[0][5].as<bool>(), rows[0][6].as<bool>(), rows[0][7].as<std::string>(), rows[0][8].as<bool>(), rows[0][9].as<std::string>(), rows[0][10].as<std::optional<std::string>>()};
}
std::optional<std::vector<DeviceGrant>> DeviceService::grants(std::string_view actorId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT 1 FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND (d.owner_id=u.id OR u.role='admin') AND d.revoked_at IS NULL AND NOT u.disabled", actorId, deviceId).empty()) {
        return std::nullopt;
    }
    std::vector<DeviceGrant> result;
    for (const auto& row : tx.exec("SELECT g.user_id,u.username,g.desktop,g.write_control,g.system_restart,g.manage_credentials,g.verification_method FROM device_grants g JOIN users u ON u.id=g.user_id WHERE device_id=? ORDER BY u.username", deviceId)) {
        result.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<bool>(), row[3].as<bool>(), row[4].as<bool>(), row[5].as<bool>(), row[6].as<std::string>()});
    }
    return result;
}
bool DeviceService::setGrant(std::string_view actorId, std::string_view deviceId, const DeviceGrant& grant, bool remove) const {
    if (!remove && ((grant.desktop != grant.writeControl) || (!grant.writeControl && (grant.systemRestart || grant.manageCredentials)))) {
        return false;
    }
    if (!remove && grant.verificationMethod != "direct" && grant.verificationMethod != "telegram" && grant.verificationMethod != "totp") {
        return false;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto device = tx.exec("SELECT d.owner_id FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND (d.owner_id=u.id OR u.role='admin') AND d.revoked_at IS NULL AND NOT u.disabled FOR UPDATE", actorId, deviceId);
    if (device.empty()) {
        return false;
    }
    const auto ownerId = device[0][0].as<std::string>();
    const auto rows = tx.exec("SELECT id FROM users WHERE username=? AND NOT disabled FOR UPDATE", grant.username);
    if (rows.empty()) {
        return false;
    }
    const auto target = rows[0][0].as<std::string>();
    if (target == ownerId && (remove || !grant.desktop || !grant.writeControl)) {
        return false;
    }
    if (!remove && target != ownerId && (grant.systemRestart || grant.manageCredentials)) {
        return false;
    }
    if (!remove && grant.verificationMethod == "telegram" && tx.exec("SELECT 1 FROM telegram_settings s JOIN telegram_accounts a ON a.user_id=?", target).empty()) {
        return false;
    }
    if (!remove && grant.verificationMethod == "totp" && tx.exec("SELECT id FROM users WHERE id=? AND totp_secret<>''", target).empty()) {
        return false;
    }
    if (remove) {
        tx.exec("DELETE FROM device_grants WHERE device_id=? AND user_id=?", deviceId, target);
    } else {
        tx.exec("INSERT INTO device_grants(device_id,user_id,desktop,vnc,ssh,write_control,system_restart,manage_credentials,verification_method) VALUES(?,?,?,false,false,?,?,?,?) ON DUPLICATE KEY UPDATE desktop=?,vnc=false,ssh=false,write_control=?,system_restart=?,manage_credentials=?,verification_method=?,revision=NEXT VALUE FOR grant_revision_sequence", deviceId, target, grant.desktop, grant.writeControl, target == ownerId, target == ownerId, grant.verificationMethod, grant.desktop, grant.writeControl, target == ownerId, target == ownerId, grant.verificationMethod);
    }
    tx.exec("UPDATE connection_verifications SET state='cancelled' WHERE device_id=? AND user_id=? AND state IN ('pending','approved')", deviceId, target);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,?,true,?)", actorId, deviceId, remove ? "device.grant.revoke" : "device.grant.save", target);
    tx.commit();
    return true;
}
} // namespace ultraterimal::server
