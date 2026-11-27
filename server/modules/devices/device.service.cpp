#include "device.service.h"

#include <stdexcept>

#include "common/database.h"
#include "device.entity.h"
#include "modules/identity/identity.service.h"
#include "ultraterimal/protocol/update.protocol.h"
namespace ultraterimal::server {
ruvia::task<DeviceInfo> DeviceService::createDevice(ruvia::db_executor db, std::string userId, std::string name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Invalid device name");
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto userRows = co_await tx.query("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", userId);
    if (userRows.empty()) {
        throw std::runtime_error("User unavailable");
    }
    const auto countRows = co_await tx.query("SELECT count(*) FROM device_grants g JOIN devices d ON d.id=g.device_id WHERE g.user_id=? AND d.revoked_at IS NULL", userId);
    if (dbField<int>(countRows[0][0]) >= 1000) {
        throw std::runtime_error("Device limit reached");
    }
    DeviceInfo result{"d-" + randomToken(), name, std::nullopt, true, false, true, true, "direct", true};
    co_await tx.execute("INSERT INTO devices(id,name,credential_hash,enrolled,owner_id) VALUES(?,?,?,false,?)", result.id, result.name, tokenHash(randomToken()), userId);
    co_await tx.execute("INSERT INTO device_grants(device_id,user_id,system_restart,manage_credentials) VALUES(?,?,true,true)", result.id, userId);
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.create',true)", userId, result.id);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<std::string>> DeviceService::createEnrollment(ruvia::db_executor db, std::string userId, std::string deviceId) const {
    const auto code = randomToken();
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto device = co_await tx.query("SELECT d.enrolled FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND d.revoked_at IS NULL AND NOT u.disabled AND d.owner_id=u.id FOR UPDATE", userId, deviceId);
    if (device.empty()) {
        co_return std::nullopt;
    }
    if (dbField<bool>(device[0][0])) {
        co_return std::string{};
    }
    co_await tx.execute("DELETE FROM enrollment_codes WHERE expires_at<UTC_TIMESTAMP(6)-INTERVAL 1 DAY");
    const auto activeCodes = co_await tx.query("SELECT count(*) FROM enrollment_codes WHERE user_id=? AND consumed_at IS NULL AND expires_at>UTC_TIMESTAMP(6)", userId);
    if (dbField<int>(activeCodes[0][0]) >= 20) {
        throw std::runtime_error("Enrollment limit reached");
    }
    co_await tx.execute("UPDATE enrollment_codes SET expires_at=UTC_TIMESTAMP(6) WHERE device_id=? AND consumed_at IS NULL", deviceId);
    co_await tx.execute("INSERT INTO enrollment_codes(code_hash,user_id,device_id,expires_at) VALUES(?,?,?,UTC_TIMESTAMP(6)+INTERVAL 10 MINUTE)", tokenHash(code), userId, deviceId);
    co_await tx.commit();
    co_return code;
}
ruvia::task<std::optional<std::string>> DeviceService::enrollmentDevice(ruvia::db_executor db, std::string code) const {
    if (!validToken(code)) {
        co_return std::nullopt;
    }
    const auto rows = co_await db.query("SELECT d.id FROM enrollment_codes e JOIN devices d ON d.id=e.device_id JOIN users u ON u.id=e.user_id WHERE e.code_hash=? AND e.consumed_at IS NULL AND e.expires_at>UTC_TIMESTAMP(6) AND NOT u.disabled AND NOT d.enrolled AND d.revoked_at IS NULL", tokenHash(code));
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return dbField<std::string>(rows[0][0]);
}
ruvia::task<std::optional<DeviceRegistration>> DeviceService::registerDevice(ruvia::db_executor db, std::string code, std::string name) const {
    if (!validToken(code) || name.empty() || name.size() > 128 || name.find('\0') != std::string_view::npos) {
        co_return std::nullopt;
    }
    const auto codeHash = tokenHash(code);
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT user_id,device_id FROM enrollment_codes WHERE code_hash=? AND device_id IS NOT NULL AND consumed_at IS NULL AND expires_at>UTC_TIMESTAMP(6) AND user_id IN (SELECT id FROM users WHERE NOT disabled) FOR UPDATE", codeHash);
    co_await tx.execute("UPDATE enrollment_codes SET consumed_at=UTC_TIMESTAMP(6) WHERE code_hash=? AND device_id IS NOT NULL AND consumed_at IS NULL AND expires_at>UTC_TIMESTAMP(6) AND user_id IN (SELECT id FROM users WHERE NOT disabled)", codeHash);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto userId = dbField<std::string>(rows[0][0]);
    DeviceRegistration result{dbField<std::string>(rows[0][1]), randomToken()};
    const auto updated = co_await tx.execute("UPDATE devices SET credential_hash=?,enrolled=true WHERE id=? AND NOT enrolled AND revoked_at IS NULL", tokenHash(result.credential), result.deviceId);
    if (updated.affected_rows() == 0) {
        co_return std::nullopt;
    }
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.register',true)", userId, result.deviceId);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<DeviceIdentity>> DeviceService::authenticate(ruvia::db_executor db, std::string credential) const {
    if (!validToken(credential)) {
        co_return std::nullopt;
    }
    const auto hash = tokenHash(credential);
    const auto found = co_await db.repository<DeviceEntity>().find_first({.where_ = DeviceEntity::column<"credential_hash">() == hash && DeviceEntity::column<"revoked_at">().is_null()});
    if (!found) {
        co_return std::nullopt;
    }
    co_return DeviceIdentity{found->get<"id">(), hash};
}
ruvia::task<bool> DeviceService::heartbeat(ruvia::db_executor db, DeviceIdentity device) const {
    const auto updated = co_await db.execute("UPDATE devices SET last_seen_at=UTC_TIMESTAMP(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", device.id, device.credentialHash);
    co_return updated.affected_rows() == 1;
}
ruvia::task<bool> DeviceService::capabilities(ruvia::db_executor db, DeviceIdentity device, std::string protobuf, std::string agentVersion) const {
    if (protobuf.size() > 65536) {
        co_return false;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT id FROM devices WHERE id=? AND credential_hash=? AND revoked_at IS NULL FOR UPDATE", device.id, device.credentialHash);
    if (rows.empty()) {
        co_return false;
    }
    co_await tx.execute("INSERT INTO device_capabilities(device_id,protocol_version,protobuf) VALUES(?,1,?) ON DUPLICATE KEY UPDATE protocol_version=1,protobuf=VALUES(protobuf),updated_at=UTC_TIMESTAMP(6)", device.id, ruvia::as_bytes(std::string_view(protobuf)));
    if (!agentVersion.empty() && agentVersion.size() <= 64 && protocol::updateVersion(agentVersion)) {
        co_await tx.execute("UPDATE devices SET agent_version=?,last_seen_at=UTC_TIMESTAMP(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", agentVersion, device.id, device.credentialHash);
    } else {
        co_await tx.execute("UPDATE devices SET last_seen_at=UTC_TIMESTAMP(6) WHERE id=? AND credential_hash=? AND revoked_at IS NULL", device.id, device.credentialHash);
    }
    co_await tx.commit();
    co_return true;
}
ruvia::task<std::vector<DeviceInfo>> DeviceService::list(ruvia::db_executor db, std::string userId, bool administration) const {
    const auto rows = co_await db.query("SELECT d.id,d.name,DATE_FORMAT(d.last_seen_at,'%Y-%m-%dT%H:%i:%s.%fZ'),d.owner_id=u.id,d.enrolled,COALESCE(g.desktop,false),COALESCE(g.write_control,false),COALESCE(g.verification_method,'direct'),d.owner_id=u.id,owner.username,d.agent_version FROM devices d JOIN users owner ON owner.id=d.owner_id JOIN users u ON u.id=? LEFT JOIN device_grants g ON g.device_id=d.id AND g.user_id=u.id WHERE ((? AND u.role='admin') OR (NOT ? AND g.user_id=u.id)) AND NOT u.disabled AND d.revoked_at IS NULL ORDER BY d.created_at LIMIT 1000", userId, administration, administration);
    std::vector<DeviceInfo> result;
    for (const auto& row : rows) {
        result.push_back({dbField<std::string>(row[0]), dbField<std::string>(row[1]), dbOptional<std::string>(row[2]), dbField<bool>(row[3]), dbField<bool>(row[4]), dbField<bool>(row[5]), dbField<bool>(row[6]), dbField<std::string>(row[7]), dbField<bool>(row[8]), dbField<std::string>(row[9]), dbOptional<std::string>(row[10])});
    }
    co_return result;
}
ruvia::task<bool> DeviceService::rename(ruvia::db_executor db, std::string userId, std::string deviceId, std::string name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        co_return false;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT d.id FROM devices d WHERE d.id=? AND d.revoked_at IS NULL AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled) FOR UPDATE", deviceId, userId, userId);
    co_await tx.execute("UPDATE devices d SET name=? WHERE d.id=? AND d.revoked_at IS NULL AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled)", name, deviceId, userId, userId);
    if (rows.empty()) {
        co_return false;
    }
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.rename',true)", userId, deviceId);
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> DeviceService::transferOwner(ruvia::db_executor db, std::string actorId, std::string deviceId, std::string targetUserId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto device = co_await tx.query("SELECT owner_id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId);
    if (device.empty()) {
        co_return false;
    }
    const auto actorRows = co_await tx.query("SELECT id FROM users WHERE id=? AND role='admin' AND NOT disabled FOR UPDATE", actorId);
    if (actorRows.empty()) {
        co_return false;
    }
    const auto oldOwnerId = dbField<std::string>(device[0][0]);
    if (oldOwnerId == targetUserId) {
        co_return false;
    }
    const auto targetRows = co_await tx.query("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", targetUserId);
    if (targetRows.empty()) {
        co_return false;
    }
    co_await tx.execute("UPDATE devices SET owner_id=? WHERE id=?", targetUserId, deviceId);
    co_await tx.repository<DeviceGrantEntity>().delete_by(DeviceGrantEntity::column<"device_id">() == deviceId && DeviceGrantEntity::column<"user_id">() == oldOwnerId);
    co_await tx.execute("INSERT INTO device_grants(device_id,user_id,desktop,vnc,ssh,write_control,system_restart,manage_credentials) VALUES(?,?,true,false,false,true,true,true) ON DUPLICATE KEY UPDATE desktop=true,vnc=false,ssh=false,write_control=true,system_restart=true,manage_credentials=true,revision=NEXT VALUE FOR grant_revision_sequence", deviceId, targetUserId);
    co_await tx.execute("UPDATE enrollment_codes SET expires_at=UTC_TIMESTAMP(6) WHERE device_id=? AND consumed_at IS NULL", deviceId);
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE device_id=? AND user_id IN (?,?) AND state IN ('pending','approved')", deviceId, oldOwnerId, targetUserId);
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.owner.transfer_from',true,?)", actorId, deviceId, oldOwnerId);
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.owner.transfer_to',true,?)", actorId, deviceId, targetUserId);
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> DeviceService::remove(ruvia::db_executor db, std::string userId, std::string deviceId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT d.id FROM devices d WHERE d.id=? AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled) FOR UPDATE", deviceId, userId, userId);
    co_await tx.execute("UPDATE devices d SET revoked_at=COALESCE(revoked_at,UTC_TIMESTAMP(6)) WHERE d.id=? AND d.owner_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=? AND NOT u.disabled)", deviceId, userId, userId);
    if (rows.empty()) {
        co_return false;
    }
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.delete',true)", userId, deviceId);
    co_await tx.commit();
    co_return true;
}
ruvia::task<std::optional<bool>> DeviceService::systemRestartPermission(ruvia::db_executor db, std::string userId, std::string deviceId) const {
    const auto rows = co_await db.query("SELECT (g.desktop AND g.write_control AND d.owner_id=g.user_id) FROM device_grants g JOIN devices d ON d.id=g.device_id JOIN users u ON u.id=g.user_id WHERE g.device_id=? AND g.user_id=? AND d.revoked_at IS NULL AND NOT u.disabled", deviceId, userId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return dbField<bool>(rows[0][0]);
}
ruvia::task<bool> DeviceService::restartDispatchAllowed(ruvia::db_executor db, std::string userId, std::string deviceId) const {
    const auto rows = co_await db.query("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE d.id=? AND g.user_id=? AND (g.desktop AND g.write_control AND d.owner_id=g.user_id) AND d.revoked_at IS NULL AND NOT u.disabled", deviceId, userId);
    co_return !rows.empty();
}
ruvia::task<std::optional<RestartOperation>> DeviceService::restartByIdempotencyKey(ruvia::db_executor db, std::string userId, std::string deviceId, std::string idempotencyKey) const {
    if (idempotencyKey.empty() || idempotencyKey.size() > 128 || idempotencyKey.find('\0') != std::string_view::npos) {
        co_return std::nullopt;
    }
    const auto rows = co_await db.query("SELECT operation_id,device_id,status,TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at),COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=? AND idempotency_key=? AND device_id=?", userId, idempotencyKey, deviceId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return RestartOperation{dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), dbField<std::string>(rows[0][2]), static_cast<std::uint64_t>(dbField<std::int64_t>(rows[0][3])), static_cast<std::uint64_t>(dbField<std::int64_t>(rows[0][4]))};
}
ruvia::task<std::optional<RestartOperation>> DeviceService::createRestart(ruvia::db_executor db, std::string userId, std::string deviceId, std::string idempotencyKey, std::uint64_t bootTimeUnixSeconds) const {
    if (idempotencyKey.empty() || idempotencyKey.size() > 128 || idempotencyKey.find('\0') != std::string_view::npos) {
        co_return std::nullopt;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto duplicate = co_await tx.query("SELECT operation_id,device_id,status,TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at),COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=? AND idempotency_key=?", userId, idempotencyKey);
    if (!duplicate.empty()) {
        if (dbField<std::string>(duplicate[0][1]) != deviceId) {
            co_await tx.commit();
            co_return std::nullopt;
        }
        RestartOperation result{dbField<std::string>(duplicate[0][0]), dbField<std::string>(duplicate[0][1]), dbField<std::string>(duplicate[0][2]), static_cast<std::uint64_t>(dbField<std::int64_t>(duplicate[0][3])), static_cast<std::uint64_t>(dbField<std::int64_t>(duplicate[0][4]))};
        co_await tx.commit();
        co_return result;
    }
    const auto device = co_await tx.query("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId);
    if (device.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT id,NULL,'device.system_restart.request',false,'denied' FROM users WHERE id=?", userId);
        co_await tx.commit();
        co_return std::nullopt;
    }
    const auto allowed = co_await tx.query("SELECT 1 FROM device_grants g JOIN devices d ON d.id=g.device_id JOIN users u ON u.id=g.user_id WHERE g.device_id=? AND g.user_id=? AND (g.desktop AND g.write_control AND d.owner_id=g.user_id) AND NOT u.disabled FOR UPDATE", deviceId, userId);
    if (allowed.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.request',false,'permission_denied')", userId, deviceId);
        co_await tx.commit();
        co_return std::nullopt;
    }
    RestartOperation result{randomToken(), deviceId, "PENDING", 0, bootTimeUnixSeconds, true};
    const auto rows = co_await tx.query("INSERT INTO restart_operations(operation_id,idempotency_key,user_id,device_id,status,expires_at,boot_time_unix_seconds) VALUES(?,?,?,?,'PENDING',UTC_TIMESTAMP(6)+INTERVAL 30 SECOND,?) RETURNING TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at)", result.id, idempotencyKey, userId, deviceId, static_cast<std::int64_t>(bootTimeUnixSeconds));
    result.expiresAtUnixSeconds = static_cast<std::uint64_t>(dbField<std::int64_t>(rows[0][0]));
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES(?,?,'device.system_restart.request',true)", userId, deviceId);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<RestartOperation>> DeviceService::restart(ruvia::db_executor db, std::string userId, std::string operationId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto expired = co_await tx.query("SELECT user_id,device_id FROM restart_operations WHERE operation_id=? AND user_id=? AND status='PENDING' AND expires_at<=UTC_TIMESTAMP(6) FOR UPDATE", operationId, userId);
    co_await tx.execute("UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=UTC_TIMESTAMP(6) WHERE operation_id=? AND user_id=? AND status='PENDING' AND expires_at<=UTC_TIMESTAMP(6)", operationId, userId);
    if (!expired.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',false,'pending_expired')", dbField<std::string>(expired[0][0]), dbField<std::string>(expired[0][1]));
    }
    const auto rows = co_await tx.query("SELECT r.operation_id,r.device_id,r.status,TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',r.expires_at),COALESCE(r.boot_time_unix_seconds,0) FROM restart_operations r JOIN users u ON u.id=r.user_id WHERE r.operation_id=? AND r.user_id=? AND NOT u.disabled", operationId, userId);
    if (rows.empty()) {
        co_await tx.commit();
        co_return std::nullopt;
    }
    RestartOperation result{dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), dbField<std::string>(rows[0][2]), static_cast<std::uint64_t>(dbField<std::int64_t>(rows[0][3])), static_cast<std::uint64_t>(dbField<std::int64_t>(rows[0][4]))};
    co_await tx.commit();
    co_return result;
}
ruvia::task<bool> DeviceService::applyRestartResult(ruvia::db_executor db, DeviceIdentity device, wire::WindowsRestartResult result) const {
    if (result.operation_id().empty() || !wire::WindowsRestartResult_Status_IsValid(result.status()) ||
        result.status() == wire::WindowsRestartResult::UNSPECIFIED) {
        co_return false;
    }
    const std::string status = result.status() == wire::WindowsRestartResult::ACCEPTED ? "ACCEPTED" : result.status() == wire::WindowsRestartResult::FAILED ? "FAILED"
                                                                                                                                                            : "UNKNOWN";
    const auto win32Error = result.status() == wire::WindowsRestartResult::FAILED ? ruvia::db_value(static_cast<std::int64_t>(result.win32_error())) : ruvia::db_value(nullptr);
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT r.user_id,r.device_id FROM restart_operations r JOIN devices d ON d.id=r.device_id WHERE r.operation_id=? AND r.device_id=? AND d.credential_hash=? AND d.revoked_at IS NULL AND r.status IN ('PENDING','UNKNOWN') AND NOT (r.status='UNKNOWN' AND ?='UNKNOWN') FOR UPDATE", result.operation_id(), device.id, device.credentialHash, status);
    co_await tx.execute("UPDATE restart_operations r JOIN devices d ON d.id=r.device_id SET r.status=?,r.result_status=?,r.win32_error=?,r.updated_at=UTC_TIMESTAMP(6) WHERE r.operation_id=? AND r.device_id=? AND d.credential_hash=? AND d.revoked_at IS NULL AND r.status IN ('PENDING','UNKNOWN') AND NOT (r.status='UNKNOWN' AND ?='UNKNOWN')", status, status, win32Error, result.operation_id(), device.id, device.credentialHash, status);
    if (!rows.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',?,?)", dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), status == "ACCEPTED", status);
    }
    co_await tx.commit();
    co_return !rows.empty();
}
ruvia::task<void> DeviceService::updateRestart(ruvia::db_executor db, std::string operationId, std::string status, std::optional<std::uint32_t> win32Error) const {
    const auto resultStatus = status == "PENDING" ? ruvia::db_value(nullptr) : ruvia::db_value(std::string_view(status));
    const auto win32Value = win32Error ? ruvia::db_value(static_cast<std::int64_t>(*win32Error)) : ruvia::db_value(nullptr);
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT user_id,device_id FROM restart_operations WHERE operation_id=? AND status='PENDING' AND ?='FAILED' FOR UPDATE", operationId, status);
    co_await tx.execute("UPDATE restart_operations SET status=?,result_status=?,win32_error=?,updated_at=UTC_TIMESTAMP(6) WHERE operation_id=? AND status='PENDING' AND ?='FAILED'", status, resultStatus, win32Value, operationId, status);
    if (!rows.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',?,?)", dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), status == "ACCEPTED", status);
    }
    co_await tx.commit();
}
ruvia::task<void> DeviceService::confirmRestart(ruvia::db_executor, std::string, std::uint64_t) const {
    // Wall-clock/uptime deltas cannot prove a Windows reboot: clock correction and
    // Agent-only restarts produce indistinguishable observations. Keep operations
    // ACCEPTED until the control protocol carries reliable boot evidence.
    co_return;
}
ruvia::task<void> DeviceService::recoverRestarts(ruvia::db_executor db) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query("SELECT user_id,device_id FROM restart_operations WHERE status='PENDING' FOR UPDATE");
    co_await tx.execute("UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=UTC_TIMESTAMP(6) WHERE status='PENDING'");
    for (const auto& row : rows) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'device.system_restart.result',false,'gateway_restarted')", dbField<std::string>(row[0]), dbField<std::string>(row[1]));
    }
    co_await tx.commit();
}
ruvia::task<bool> DeviceService::canConfigureWindowsLogin(ruvia::db_executor db, std::string userId, std::string deviceId) const {
    const auto rows = co_await db.query("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE g.user_id=? AND d.id=? AND NOT u.disabled AND d.revoked_at IS NULL AND d.owner_id=u.id", userId, deviceId);
    co_return !rows.empty();
}
ruvia::task<void> DeviceService::auditWindowsLogin(ruvia::db_executor db, std::string userId, std::string deviceId, std::string action, bool success) const {
    if (action != "save" && action != "clear") {
        co_return;
    }
    AuditEventEntity event;
    event.set<"user_id">(userId).set<"device_id">(deviceId).set<"event_type">(std::string("device.windows_login.") + action).set<"success">(success);
    co_await db.repository<AuditEventEntity>().insert(event);
}
ruvia::task<std::optional<DeviceInfo>> DeviceService::find(ruvia::db_executor db, std::string userId, std::string deviceId, bool administration) const {
    const auto rows = co_await db.query("SELECT d.id,d.name,DATE_FORMAT(d.last_seen_at,'%Y-%m-%dT%H:%i:%s.%fZ'),d.owner_id=u.id,d.enrolled,COALESCE(g.desktop,false),COALESCE(g.write_control,false),COALESCE(g.verification_method,'direct'),d.owner_id=u.id,owner.username,d.agent_version FROM devices d JOIN users owner ON owner.id=d.owner_id JOIN users u ON u.id=? LEFT JOIN device_grants g ON g.device_id=d.id AND g.user_id=u.id WHERE ((? AND u.role='admin') OR (NOT ? AND g.user_id=u.id)) AND d.id=? AND NOT u.disabled AND d.revoked_at IS NULL", userId, administration, administration, deviceId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return DeviceInfo{dbField<std::string>(rows[0][0]), dbField<std::string>(rows[0][1]), dbOptional<std::string>(rows[0][2]), dbField<bool>(rows[0][3]), dbField<bool>(rows[0][4]), dbField<bool>(rows[0][5]), dbField<bool>(rows[0][6]), dbField<std::string>(rows[0][7]), dbField<bool>(rows[0][8]), dbField<std::string>(rows[0][9]), dbOptional<std::string>(rows[0][10])};
}
ruvia::task<std::optional<std::vector<DeviceGrant>>> DeviceService::grants(ruvia::db_executor db, std::string actorId, std::string deviceId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto allowed = co_await tx.query("SELECT 1 FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND (d.owner_id=u.id OR u.role='admin') AND d.revoked_at IS NULL AND NOT u.disabled", actorId, deviceId);
    if (allowed.empty()) {
        co_return std::nullopt;
    }
    std::vector<DeviceGrant> result;
    const auto rows = co_await tx.query("SELECT g.user_id,u.username,g.desktop,g.write_control,g.system_restart,g.manage_credentials,g.verification_method FROM device_grants g JOIN users u ON u.id=g.user_id WHERE device_id=? ORDER BY u.username", deviceId);
    for (const auto& row : rows) {
        result.push_back({dbField<std::string>(row[0]), dbField<std::string>(row[1]), dbField<bool>(row[2]), dbField<bool>(row[3]), dbField<bool>(row[4]), dbField<bool>(row[5]), dbField<std::string>(row[6])});
    }
    co_await tx.commit();
    co_return result;
}
ruvia::task<bool> DeviceService::setGrant(ruvia::db_executor db, std::string actorId, std::string deviceId, DeviceGrant grant, bool remove) const {
    if (!remove && ((grant.desktop != grant.writeControl) || (!grant.writeControl && (grant.systemRestart || grant.manageCredentials)))) {
        co_return false;
    }
    if (!remove && grant.verificationMethod != "direct" && grant.verificationMethod != "telegram" && grant.verificationMethod != "totp") {
        co_return false;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto device = co_await tx.query("SELECT d.owner_id FROM devices d JOIN users u ON u.id=? WHERE d.id=? AND (d.owner_id=u.id OR u.role='admin') AND d.revoked_at IS NULL AND NOT u.disabled FOR UPDATE", actorId, deviceId);
    if (device.empty()) {
        co_return false;
    }
    const auto ownerId = dbField<std::string>(device[0][0]);
    const auto rows = co_await tx.query("SELECT id FROM users WHERE username=? AND NOT disabled FOR UPDATE", grant.username);
    if (rows.empty()) {
        co_return false;
    }
    const auto target = dbField<std::string>(rows[0][0]);
    if (target == ownerId && (remove || !grant.desktop || !grant.writeControl)) {
        co_return false;
    }
    if (!remove && target != ownerId && (grant.systemRestart || grant.manageCredentials)) {
        co_return false;
    }
    if (!remove && grant.verificationMethod == "telegram") {
        const auto tg = co_await tx.query("SELECT 1 FROM telegram_settings s JOIN telegram_accounts a ON a.user_id=?", target);
        if (tg.empty()) {
            co_return false;
        }
    }
    if (!remove && grant.verificationMethod == "totp") {
        const auto totp = co_await tx.query("SELECT id FROM users WHERE id=? AND totp_secret<>''", target);
        if (totp.empty()) {
            co_return false;
        }
    }
    if (remove) {
        co_await tx.repository<DeviceGrantEntity>().delete_by(DeviceGrantEntity::column<"device_id">() == deviceId && DeviceGrantEntity::column<"user_id">() == target);
    } else {
        co_await tx.execute("INSERT INTO device_grants(device_id,user_id,desktop,vnc,ssh,write_control,system_restart,manage_credentials,verification_method) VALUES(?,?,?,false,false,?,?,?,?) ON DUPLICATE KEY UPDATE desktop=?,vnc=false,ssh=false,write_control=?,system_restart=?,manage_credentials=?,verification_method=?,revision=NEXT VALUE FOR grant_revision_sequence", deviceId, target, grant.desktop, grant.writeControl, target == ownerId, target == ownerId, grant.verificationMethod, grant.desktop, grant.writeControl, target == ownerId, target == ownerId, grant.verificationMethod);
    }
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE device_id=? AND user_id=? AND state IN ('pending','approved')", deviceId, target);
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,?,true,?)", actorId, deviceId, remove ? "device.grant.revoke" : "device.grant.save", target);
    co_await tx.commit();
    co_return true;
}
} // namespace ultraterimal::server
