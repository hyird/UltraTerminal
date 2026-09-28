#include "device.service.h"

#include <stdexcept>

#include "modules/database/database.service.h"
#include "modules/identity/identity.service.h"
namespace ultraterimal::server {
DeviceInfo DeviceService::createDevice(std::string_view userId, std::string_view name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Invalid device name");
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    if (tx.exec("SELECT id FROM users WHERE id=$1 AND NOT disabled FOR UPDATE", pqxx::params(tx, userId)).empty()) {
        throw std::runtime_error("User unavailable");
    }
    if (tx.exec("SELECT count(*) FROM device_grants g JOIN devices d ON d.id=g.device_id WHERE g.user_id=$1 AND d.revoked_at IS NULL", pqxx::params(tx, userId))[0][0].as<int>() >= 1000) {
        throw std::runtime_error("Device limit reached");
    }
    DeviceInfo result{"d-" + randomToken(), std::string(name), std::nullopt, true, false};
    tx.exec("INSERT INTO devices(id,name,credential_hash,enrolled) VALUES($1,$2,$3,false)", pqxx::params(tx, result.id, result.name, tokenHash(randomToken())));
    tx.exec("INSERT INTO device_grants(device_id,user_id) VALUES($1,$2)", pqxx::params(tx, result.id, userId));
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.create',true)", pqxx::params(tx, userId, result.id));
    tx.commit();
    return result;
}
std::optional<std::string> DeviceService::createEnrollment(std::string_view userId, std::string_view deviceId) const {
    const auto code = randomToken();
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto device = tx.exec("SELECT d.enrolled FROM devices d JOIN users u ON u.id=$1 WHERE d.id=$2 AND d.revoked_at IS NULL AND NOT u.disabled AND EXISTS (SELECT 1 FROM audit_events a WHERE a.device_id=d.id AND a.user_id=u.id AND a.event_type IN ('device.create','device.register') AND a.success) FOR UPDATE OF d", pqxx::params(tx, userId, deviceId));
    if (device.empty()) {
        return std::nullopt;
    }
    if (device[0][0].as<bool>()) {
        return std::string{};
    }
    tx.exec("DELETE FROM enrollment_codes WHERE expires_at<now()-interval '1 day'");
    if (tx.exec("SELECT count(*) FROM enrollment_codes WHERE user_id=$1 AND consumed_at IS NULL AND expires_at>now()", pqxx::params(tx, userId))[0][0].as<int>() >= 20) {
        throw std::runtime_error("Enrollment limit reached");
    }
    tx.exec("UPDATE enrollment_codes SET expires_at=now() WHERE device_id=$1 AND consumed_at IS NULL", pqxx::params(tx, deviceId));
    tx.exec("INSERT INTO enrollment_codes(code_hash,user_id,device_id,expires_at) VALUES($1,$2,$3,now()+interval '10 minutes')", pqxx::params(tx, tokenHash(code), userId, deviceId));
    tx.commit();
    return code;
}
std::optional<std::string> DeviceService::enrollmentDevice(std::string_view code) const {
    if (!validToken(code)) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT d.id FROM enrollment_codes e JOIN devices d ON d.id=e.device_id JOIN users u ON u.id=e.user_id WHERE e.code_hash=$1 AND e.consumed_at IS NULL AND e.expires_at>now() AND NOT u.disabled AND NOT d.enrolled AND d.revoked_at IS NULL", pqxx::params(tx, tokenHash(code)));
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
    pqxx::work tx(connection);
    const auto rows = tx.exec("UPDATE enrollment_codes SET consumed_at=now() WHERE code_hash=$1 AND device_id IS NOT NULL AND consumed_at IS NULL AND expires_at>now() AND user_id IN (SELECT id FROM users WHERE NOT disabled) RETURNING user_id,device_id", pqxx::params(tx, tokenHash(code)));
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto userId = rows[0][0].as<std::string>();
    DeviceRegistration result{rows[0][1].as<std::string>(), randomToken()};
    if (tx.exec("UPDATE devices SET credential_hash=$2,enrolled=true WHERE id=$1 AND NOT enrolled AND revoked_at IS NULL RETURNING id", pqxx::params(tx, result.deviceId, tokenHash(result.credential))).empty()) {
        return std::nullopt;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.register',true)", pqxx::params(tx, userId, result.deviceId));
    tx.commit();
    return result;
}
std::optional<DeviceIdentity> DeviceService::authenticate(std::string_view credential) const {
    if (!validToken(credential)) {
        return std::nullopt;
    }
    const auto hash = tokenHash(credential);
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT id FROM devices WHERE credential_hash=$1 AND revoked_at IS NULL", pqxx::params(tx, hash));
    if (rows.empty()) {
        return std::nullopt;
    }
    return DeviceIdentity{rows[0][0].as<std::string>(), hash};
}
bool DeviceService::heartbeat(const DeviceIdentity& device) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("UPDATE devices SET last_seen_at=now() WHERE id=$1 AND credential_hash=$2 AND revoked_at IS NULL RETURNING id", pqxx::params(tx, device.id, device.credentialHash));
    tx.commit();
    return !rows.empty();
}
bool DeviceService::capabilities(const DeviceIdentity& device, std::string_view protobuf) const {
    if (protobuf.size() > 65536) {
        return false;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=$1 AND credential_hash=$2 AND revoked_at IS NULL FOR UPDATE", pqxx::params(tx, device.id, device.credentialHash)).empty()) {
        return false;
    }
    const pqxx::bytes_view bytes(reinterpret_cast<const std::byte*>(protobuf.data()), protobuf.size());
    tx.exec("INSERT INTO device_capabilities(device_id,protocol_version,protobuf) VALUES($1,1,$2) ON CONFLICT(device_id) DO UPDATE SET protocol_version=1,protobuf=excluded.protobuf,updated_at=now()", pqxx::params(tx, device.id, bytes));
    tx.commit();
    return true;
}
std::vector<DeviceInfo> DeviceService::list(std::string_view userId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT d.id,d.name,d.last_seen_at::text,EXISTS (SELECT 1 FROM audit_events a WHERE a.device_id=d.id AND a.user_id=u.id AND a.event_type IN ('device.create','device.register') AND a.success),d.enrolled FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE g.user_id=$1 AND NOT u.disabled AND d.revoked_at IS NULL ORDER BY d.created_at LIMIT 1000", pqxx::params(tx, userId));
    std::vector<DeviceInfo> result;
    for (const auto& row : rows) {
        result.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::optional<std::string>>(), row[3].as<bool>(), row[4].as<bool>()});
    }
    return result;
}
bool DeviceService::rename(std::string_view userId, std::string_view deviceId, std::string_view name) const {
    if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("UPDATE devices d SET name=$3 WHERE d.id=$2 AND d.revoked_at IS NULL AND EXISTS (SELECT 1 FROM audit_events a JOIN users u ON u.id=a.user_id WHERE a.device_id=d.id AND a.user_id=$1 AND a.event_type IN ('device.create','device.register') AND a.success AND NOT u.disabled) RETURNING d.id", pqxx::params(tx, userId, deviceId, name));
    if (rows.empty()) {
        return false;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.rename',true)", pqxx::params(tx, userId, deviceId));
    tx.commit();
    return true;
}
bool DeviceService::remove(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("UPDATE devices d SET revoked_at=COALESCE(revoked_at,now()) WHERE d.id=$2 AND EXISTS (SELECT 1 FROM audit_events a JOIN users u ON u.id=a.user_id WHERE a.device_id=d.id AND a.user_id=$1 AND a.event_type IN ('device.create','device.register') AND a.success AND NOT u.disabled) RETURNING d.id", pqxx::params(tx, userId, deviceId));
    if (rows.empty()) {
        return false;
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.delete',true)", pqxx::params(tx, userId, deviceId));
    tx.commit();
    return true;
}
std::optional<bool> DeviceService::systemRestartPermission(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT (g.desktop AND g.write_control) FROM device_grants g JOIN devices d ON d.id=g.device_id JOIN users u ON u.id=g.user_id WHERE g.device_id=$1 AND g.user_id=$2 AND d.revoked_at IS NULL AND NOT u.disabled AND EXISTS (SELECT 1 FROM audit_events a WHERE a.device_id=d.id AND a.user_id=u.id AND a.event_type IN ('device.create','device.register') AND a.success)", pqxx::params(tx, deviceId, userId));
    if (rows.empty()) {
        return std::nullopt;
    }
    return rows[0][0].as<bool>();
}
bool DeviceService::restartDispatchAllowed(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    return !tx.exec("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE d.id=$1 AND g.user_id=$2 AND (g.desktop AND g.write_control) AND d.revoked_at IS NULL AND NOT u.disabled AND EXISTS (SELECT 1 FROM audit_events a WHERE a.device_id=d.id AND a.user_id=u.id AND a.event_type IN ('device.create','device.register') AND a.success)", pqxx::params(tx, deviceId, userId)).empty();
}
std::optional<RestartOperation> DeviceService::restartByIdempotencyKey(std::string_view userId, std::string_view deviceId, std::string_view idempotencyKey) const {
    if (idempotencyKey.empty() || idempotencyKey.size() > 128 || idempotencyKey.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT operation_id,device_id,status,extract(epoch from expires_at)::bigint,COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=$1 AND idempotency_key=$2 AND device_id=$3", pqxx::params(tx, userId, idempotencyKey, deviceId));
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
    pqxx::work tx(connection);
    const auto duplicate = tx.exec("SELECT operation_id,device_id,status,extract(epoch from expires_at)::bigint,COALESCE(boot_time_unix_seconds,0) FROM restart_operations WHERE user_id=$1 AND idempotency_key=$2", pqxx::params(tx, userId, idempotencyKey));
    if (!duplicate.empty()) {
        if (duplicate[0][1].as<std::string>() != deviceId) {
            tx.commit();
            return std::nullopt;
        }
        RestartOperation result{duplicate[0][0].as<std::string>(), duplicate[0][1].as<std::string>(), duplicate[0][2].as<std::string>(), static_cast<std::uint64_t>(duplicate[0][3].as<std::int64_t>()), static_cast<std::uint64_t>(duplicate[0][4].as<std::int64_t>())};
        tx.commit();
        return result;
    }
    const auto device = tx.exec("SELECT id FROM devices WHERE id=$1 AND revoked_at IS NULL FOR UPDATE", pqxx::params(tx, deviceId));
    if (device.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT id,NULL,'device.system_restart.request',false,'denied' FROM users WHERE id=$1", pqxx::params(tx, userId));
        tx.commit();
        return std::nullopt;
    }
    const auto allowed = tx.exec("SELECT 1 FROM device_grants g JOIN users u ON u.id=g.user_id WHERE g.device_id=$1 AND g.user_id=$2 AND (g.desktop AND g.write_control) AND NOT u.disabled AND EXISTS (SELECT 1 FROM audit_events a WHERE a.device_id=g.device_id AND a.user_id=g.user_id AND a.event_type IN ('device.create','device.register') AND a.success) FOR UPDATE OF g", pqxx::params(tx, deviceId, userId));
    if (allowed.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES($1,$2,'device.system_restart.request',false,'permission_denied')", pqxx::params(tx, userId, deviceId));
        tx.commit();
        return std::nullopt;
    }
    RestartOperation result{randomToken(), std::string(deviceId), "PENDING", 0, bootTimeUnixSeconds, true};
    const auto rows = tx.exec("INSERT INTO restart_operations(operation_id,idempotency_key,user_id,device_id,status,expires_at,boot_time_unix_seconds) VALUES($1,$2,$3,$4,'PENDING',now()+interval '30 seconds',$5) RETURNING extract(epoch from expires_at)::bigint", pqxx::params(tx, result.id, idempotencyKey, userId, deviceId, bootTimeUnixSeconds));
    result.expiresAtUnixSeconds = static_cast<std::uint64_t>(rows[0][0].as<std::int64_t>());
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.system_restart.request',true)", pqxx::params(tx, userId, deviceId));
    tx.commit();
    return result;
}
std::optional<RestartOperation> DeviceService::restart(std::string_view userId, std::string_view operationId) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto expired = tx.exec("UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=now() WHERE operation_id=$1 AND user_id=$2 AND status='PENDING' AND expires_at<=now() RETURNING user_id,device_id", pqxx::params(tx, operationId, userId));
    if (!expired.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES($1,$2,'device.system_restart.result',false,'pending_expired')", pqxx::params(tx, expired[0][0].as<std::string>(), expired[0][1].as<std::string>()));
    }
    const auto rows = tx.exec("SELECT r.operation_id,r.device_id,r.status,extract(epoch from r.expires_at)::bigint,COALESCE(r.boot_time_unix_seconds,0) FROM restart_operations r JOIN users u ON u.id=r.user_id WHERE r.operation_id=$1 AND r.user_id=$2 AND NOT u.disabled", pqxx::params(tx, operationId, userId));
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
    pqxx::work tx(connection);
    const auto rows = tx.exec(
        "UPDATE restart_operations r SET status=$3,result_status=$3,win32_error=$4,updated_at=now() "
        "FROM devices d WHERE r.operation_id=$1 AND r.device_id=$2 AND d.id=r.device_id "
        "AND d.credential_hash=$5 AND d.revoked_at IS NULL AND r.status IN ('PENDING','UNKNOWN') "
        "AND NOT (r.status='UNKNOWN' AND $3='UNKNOWN') RETURNING r.user_id,r.device_id",
        pqxx::params(tx, result.operation_id(), device.id, status,
                     result.status() == wire::WindowsRestartResult::FAILED ? std::optional<std::uint32_t>(result.win32_error()) : std::nullopt,
                     device.credentialHash));
    if (!rows.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES($1,$2,'device.system_restart.result',$4,$3)",
                pqxx::params(tx, rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), status, status == std::string_view("ACCEPTED")));
    }
    tx.commit();
    return !rows.empty();
}
void DeviceService::updateRestart(std::string_view operationId, std::string_view status, std::optional<std::uint32_t> win32Error) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("UPDATE restart_operations SET status=$2,result_status=$3,win32_error=$4,updated_at=now() WHERE operation_id=$1 AND status='PENDING' AND $2='FAILED' RETURNING user_id,device_id", pqxx::params(tx, operationId, status, status == "PENDING" ? std::optional<std::string>{} : std::optional<std::string>{status}, win32Error));
    if (!rows.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES($1,$2,'device.system_restart.result',$4,$3)", pqxx::params(tx, rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), status, status == "ACCEPTED"));
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
    pqxx::work tx(connection);
    tx.exec("WITH recovered AS (UPDATE restart_operations SET status='UNKNOWN',result_status='UNKNOWN',updated_at=now() WHERE status='PENDING' RETURNING user_id,device_id) INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT user_id,device_id,'device.system_restart.result',false,'gateway_restarted' FROM recovered");
    tx.commit();
}
bool DeviceService::canConfigureWindowsLogin(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    return !tx.exec("SELECT 1 FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE g.user_id=$1 AND d.id=$2 AND NOT u.disabled AND d.revoked_at IS NULL AND g.desktop AND g.write_control", pqxx::params(tx, userId, deviceId)).empty();
}
void DeviceService::auditWindowsLogin(std::string_view userId, std::string_view deviceId, std::string_view action, bool success) const {
    if (action != "save" && action != "clear") {
        return;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,$3,$4)", pqxx::params(tx, userId, deviceId, std::string("device.windows_login.") + std::string(action), success));
    tx.commit();
}
std::optional<DeviceInfo> DeviceService::find(std::string_view userId, std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT d.id,d.name,d.last_seen_at::text FROM devices d JOIN device_grants g ON g.device_id=d.id JOIN users u ON u.id=g.user_id WHERE g.user_id=$1 AND d.id=$2 AND NOT u.disabled AND d.revoked_at IS NULL", pqxx::params(tx, userId, deviceId));
    if (rows.empty()) {
        return std::nullopt;
    }
    return DeviceInfo{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::optional<std::string>>()};
}
} // namespace ultraterimal::server
