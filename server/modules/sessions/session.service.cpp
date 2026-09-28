#include "session.service.h"

#include <array>

#include "modules/database/database.service.h"
#include "modules/identity/identity.service.h"

namespace ultraterimal::server {
namespace {
bool validRequest(const SessionRequest& request) {
    if (request.userId.empty() || request.userId.size() > 128 || request.deviceId.empty() || request.deviceId.size() > 128 ||
        request.kind < wire::DESKTOP || request.kind > wire::SSH) {
        return false;
    }
    if (request.kind == wire::DESKTOP) {
        if (request.writeControl ? request.desktopInputContract != 2 : request.desktopInputContract != 0) {
            return false;
        }
    } else if (request.desktopInputContract) {
        return false;
    }
    return request.kind != wire::SSH || request.writeControl;
}
bool permission(pqxx::work& tx, const SessionRequest& request) {
    // Device, user, grant, session is the lock order for all session transactions.
    if (tx.exec("SELECT id FROM devices WHERE id=$1 AND revoked_at IS NULL FOR UPDATE", pqxx::params(tx, request.deviceId)).empty() ||
        tx.exec("SELECT id FROM users WHERE id=$1 AND NOT disabled FOR NO KEY UPDATE", pqxx::params(tx, request.userId)).empty()) {
        return false;
    }
    const auto grants = tx.exec("SELECT desktop,vnc,ssh,write_control FROM device_grants WHERE device_id=$1 AND user_id=$2 FOR SHARE", pqxx::params(tx, request.deviceId, request.userId));
    return !grants.empty() && grants[0][static_cast<int>(request.kind) - 1].as<bool>() && (!request.writeControl || grants[0][3].as<bool>());
}
void expirePending(pqxx::work& tx, std::string_view deviceId) {
    tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason='setup_expired' WHERE device_id=$1 AND state='pending' AND setup_expires_at<=now() RETURNING id,user_id,device_id) "
            "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',false,'setup_expired' FROM ended",
            pqxx::params(tx, deviceId));
}
void auditDenied(pqxx::work& tx, const SessionRequest& request, std::string_view reason) {
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT u.id,d.id,'session.create',false,$3 FROM users u LEFT JOIN devices d ON d.id=$2 WHERE u.id=$1", pqxx::params(tx, request.userId, request.deviceId, reason));
    tx.commit();
}
SessionRequest requestFrom(const pqxx::row_ref& row) {
    return {row[0].as<std::string>(), row[1].as<std::string>(), static_cast<wire::SessionKind>(row[2].as<int>()), row[3].as<bool>()};
}
constexpr std::string_view kSessionFields = "SELECT user_id,device_id,kind,write_control FROM remote_sessions WHERE id=$1";
std::string_view endReason(SessionEnd reason) {
    switch (reason) {
    case SessionEnd::kUser:
        return "user_closed";
    case SessionEnd::kAgentClosed:
        return "agent_closed";
    case SessionEnd::kAgentDisconnected:
        return "agent_disconnected";
    case SessionEnd::kPermissionRevoked:
        return "permission_revoked";
    case SessionEnd::kFailure:
        return "session_failed";
    }
    return "session_failed";
}
} // namespace
std::expected<IssuedSession, SessionFailure> SessionService::create(const SessionRequest& request) const {
    if (!validRequest(request)) {
        return std::unexpected(SessionFailure::kInvalid);
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    if (!permission(tx, request)) {
        auditDenied(tx, request, "permission_denied");
        return std::unexpected(SessionFailure::kDenied);
    }
    if (request.kind == wire::DESKTOP && request.writeControl && !request.agentInputContractSupported) {
        auditDenied(tx, request, "desktop_input_upgrade_required");
        return std::unexpected(SessionFailure::kIncompatible);
    }
    expirePending(tx, request.deviceId);
    const auto count = tx.exec("SELECT count(*) FROM remote_sessions WHERE user_id=$1 AND device_id<>$2 AND (state='active' OR (state='pending' AND setup_expires_at>now()))", pqxx::params(tx, request.userId, request.deviceId));
    if (count[0][0].as<int>() >= 16) {
        auditDenied(tx, request, "session_limit");
        return std::unexpected(SessionFailure::kLimit);
    }
    if (!request.preempt && !tx.exec("SELECT id FROM remote_sessions WHERE device_id=$1 AND state<>'closed'", pqxx::params(tx, request.deviceId)).empty()) {
        auditDenied(tx, request, "device_busy");
        return std::unexpected(SessionFailure::kBusy);
    }
    IssuedSession result{"s-" + randomToken(), {}, {}};
    const auto preempted = tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason='preempted' WHERE device_id=$1 AND state<>'closed' RETURNING id,user_id,device_id) "
                                   "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',false,'preempted' FROM ended RETURNING session_id",
                                   pqxx::params(tx, request.deviceId));
    result.preemptedSessions.reserve(preempted.size());
    for (const auto& row : preempted) {
        result.preemptedSessions.push_back(row[0].as<std::string>());
    }
    tx.exec("INSERT INTO remote_sessions(id,user_id,device_id,kind,write_control,setup_expires_at) VALUES($1,$2,$3,$4,$5,now()+interval '60 seconds')",
            pqxx::params(tx, result.id, request.userId, request.deviceId, static_cast<int>(request.kind), request.writeControl));
    const std::array purposes{wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO, wire::VNC_STREAM, wire::SSH_STREAM};
    for (auto purpose : purposes) {
        if (!protocol::purposeMatches(request.kind, purpose)) {
            continue;
        }
        IssuedChannel channel{"c-" + randomToken(), purpose, randomToken(), randomToken()};
        tx.exec("INSERT INTO session_channels(id,session_id,purpose,browser_ticket_hash,agent_ticket_hash,expires_at) VALUES($1,$2,$3,$4,$5,now()+interval '60 seconds')",
                pqxx::params(tx, channel.id, result.id, static_cast<int>(purpose), tokenHash(channel.browserTicket), tokenHash(channel.agentTicket)));
        result.channels.push_back(std::move(channel));
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES($1,$2,$3,'session.create',true)", pqxx::params(tx, request.userId, request.deviceId, result.id));
    tx.commit();
    return result;
}
std::optional<ChannelAuthorization> SessionService::consumeBrowser(std::string_view userId, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    if (userId.empty()) {
        return std::nullopt;
    }
    return consume(userId, nullptr, sessionId, channelId, purpose, ticket);
}
std::optional<ChannelAuthorization> SessionService::consumeAgent(const DeviceIdentity& device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    return consume({}, &device, sessionId, channelId, purpose, ticket);
}
std::optional<ChannelAuthorization> SessionService::consume(std::string_view userId, const DeviceIdentity* device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    if (!validToken(ticket) || sessionId.size() > 128 || channelId.size() > 128) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec(kSessionFields, pqxx::params(tx, sessionId));
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto request = requestFrom(rows[0]);
    if ((device ? device->id != request.deviceId : userId != request.userId) || !protocol::purposeMatches(request.kind, purpose) || !permission(tx, request)) {
        return std::nullopt;
    }
    if (device && tx.exec("SELECT id FROM devices WHERE id=$1 AND credential_hash=$2", pqxx::params(tx, device->id, device->credentialHash)).empty()) {
        return std::nullopt;
    }
    const auto session = tx.exec("SELECT id FROM remote_sessions WHERE id=$1 AND (state='active' OR (state='pending' AND setup_expires_at>now())) FOR UPDATE", pqxx::params(tx, sessionId));
    if (session.empty()) {
        return std::nullopt;
    }
    const auto query = device ? "UPDATE session_channels SET agent_consumed_at=now() WHERE id=$1 AND session_id=$2 AND purpose=$3 AND agent_ticket_hash=$4 AND agent_consumed_at IS NULL AND expires_at>now() RETURNING id" : "UPDATE session_channels SET browser_consumed_at=now() WHERE id=$1 AND session_id=$2 AND purpose=$3 AND browser_ticket_hash=$4 AND browser_consumed_at IS NULL AND expires_at>now() RETURNING id";
    if (tx.exec(query, pqxx::params(tx, channelId, sessionId, static_cast<int>(purpose), tokenHash(ticket))).empty()) {
        return std::nullopt;
    }
    tx.exec("WITH activated AS (UPDATE remote_sessions SET state='active' WHERE id=$1 AND state='pending' AND NOT EXISTS (SELECT 1 FROM session_channels WHERE session_id=$1 AND (browser_consumed_at IS NULL OR agent_consumed_at IS NULL)) RETURNING id,user_id,device_id) "
            "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) SELECT user_id,device_id,id,'session.active',true FROM activated",
            pqxx::params(tx, sessionId));
    tx.commit();
    return ChannelAuthorization{std::string(sessionId), std::string(channelId), request.userId, request.deviceId, request.kind, purpose, request.writeControl};
}
bool SessionService::close(std::string_view userId, std::string_view sessionId, SessionEnd reason) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec(kSessionFields, pqxx::params(tx, sessionId));
    if (rows.empty() || rows[0][0].as<std::string>() != userId) {
        return false;
    }
    tx.exec("SELECT id FROM devices WHERE id=$1 FOR UPDATE", pqxx::params(tx, rows[0][1].as<std::string>()));
    const auto ended = tx.exec("UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason=$2 WHERE id=$1 AND state<>'closed' RETURNING device_id", pqxx::params(tx, sessionId, endReason(reason)));
    if (!ended.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES($1,$2,$3,'session.end',$4,$5)", pqxx::params(tx, userId, ended[0][0].as<std::string>(), sessionId, reason == SessionEnd::kUser, endReason(reason)));
    }
    tx.commit();
    return true;
}
bool SessionService::authorized(std::string_view sessionId) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec(kSessionFields, pqxx::params(tx, sessionId));
    if (rows.empty() || !permission(tx, requestFrom(rows[0]))) {
        return false;
    }
    const bool allowed = !tx.exec("SELECT id FROM remote_sessions WHERE id=$1 AND (state='active' OR (state='pending' AND setup_expires_at>now()))", pqxx::params(tx, sessionId)).empty();
    tx.commit();
    return allowed;
}
void SessionService::recoverAfterRestart() const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason='gateway_restarted' WHERE state<>'closed' RETURNING id,user_id,device_id) "
            "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',false,'gateway_restarted' FROM ended");
    tx.commit();
}
std::optional<SessionInfo> SessionService::get(std::string_view userId, std::string_view sessionId) const {
    auto connection = openDatabase(database_);
    pqxx::read_transaction tx(connection);
    const auto rows = tx.exec("SELECT id,device_id,state,coalesce(end_reason,'') FROM remote_sessions WHERE id=$1 AND user_id=$2", pqxx::params(tx, sessionId, userId));
    if (rows.empty()) {
        return std::nullopt;
    }
    return SessionInfo{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::string>(), rows[0][3].as<std::string>()};
}
bool SessionService::closeForDevice(const DeviceIdentity& device, std::string_view sessionId, SessionEnd reason) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=$1 AND credential_hash=$2 FOR UPDATE", pqxx::params(tx, device.id, device.credentialHash)).empty()) {
        return false;
    }
    const auto rows = tx.exec("SELECT id FROM remote_sessions WHERE id=$1 AND device_id=$2", pqxx::params(tx, sessionId, device.id));
    if (rows.empty()) {
        return false;
    }
    tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason=$3 WHERE id=$1 AND device_id=$2 AND state<>'closed' RETURNING id,user_id,device_id) "
            "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',$4,$3 FROM ended",
            pqxx::params(tx, sessionId, device.id, endReason(reason), reason == SessionEnd::kAgentClosed));
    tx.commit();
    return true;
}
std::vector<std::string> SessionService::revokeDevice(std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    const auto rows = tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason='device_deleted' WHERE device_id=$1 AND state<>'closed' AND EXISTS (SELECT 1 FROM devices WHERE id=$1 AND revoked_at IS NOT NULL) RETURNING id,user_id,device_id) INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',false,'device_deleted' FROM ended RETURNING session_id", pqxx::params(tx, deviceId));
    std::vector<std::string> result;
    for (const auto& row : rows) {
        result.push_back(row[0].as<std::string>());
    }
    tx.commit();
    return result;
}
void SessionService::disconnectDevice(const DeviceIdentity& device) const {
    auto connection = openDatabase(database_);
    pqxx::work tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=$1 AND credential_hash=$2 FOR UPDATE", pqxx::params(tx, device.id, device.credentialHash)).empty()) {
        return;
    }
    tx.exec("WITH ended AS (UPDATE remote_sessions SET state='closed',ended_at=now(),end_reason='agent_disconnected' WHERE device_id=$1 AND state<>'closed' RETURNING id,user_id,device_id) "
            "INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) SELECT user_id,device_id,id,'session.end',false,'agent_disconnected' FROM ended",
            pqxx::params(tx, device.id));
    tx.commit();
}
} // namespace ultraterimal::server
