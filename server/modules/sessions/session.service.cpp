#include "session.service.h"

#include <array>

#include "modules/database/database.service.h"
#include "modules/identity/identity.service.h"
#include "modules/telegram/telegram.service.h"

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
bool permission(DatabaseTransaction& tx, const SessionRequest& request) {
    // Device, user, grant, session is the lock order for all session transactions.
    if (tx.exec("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", request.deviceId).empty() ||
        tx.exec("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", request.userId).empty()) {
        return false;
    }
    const auto grants = tx.exec("SELECT desktop,vnc,ssh,write_control,revision FROM device_grants WHERE device_id=? AND user_id=? LOCK IN SHARE MODE", request.deviceId, request.userId);
    return validToken(request.loginHash) && !tx.exec("SELECT 1 FROM login_sessions WHERE token_hash=? AND user_id=? AND expires_at>now(6)", request.loginHash, request.userId).empty() && !grants.empty() && (!request.grantRevision || request.grantRevision == grants[0][4].as<std::int64_t>()) && grants[0][static_cast<int>(request.kind) - 1].as<bool>() && (!request.writeControl || grants[0][3].as<bool>());
}
DatabaseResult endSessions(DatabaseTransaction& tx, DatabaseResult rows, std::string_view reason, bool success = false) {
    for (const auto& row : rows) {
        const auto id = row[0].as<std::string>();
        tx.exec("UPDATE remote_sessions SET state='closed',ended_at=now(6),end_reason=? WHERE id=?", reason, id);
        tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,'session.end',?,?)", row[1].as<std::string>(), row[2].as<std::string>(), id, success, reason);
    }
    return rows;
}
void expirePending(DatabaseTransaction& tx, std::string_view deviceId) {
    endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state='pending' AND setup_expires_at<=now(6) FOR UPDATE", deviceId), "setup_expired");
}
void auditDenied(DatabaseTransaction& tx, const SessionRequest& request, std::string_view reason) {
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT u.id,d.id,'session.create',false,? FROM users u LEFT JOIN devices d ON d.id=? WHERE u.id=?", reason, request.deviceId, request.userId);
    tx.commit();
}
SessionRequest requestFrom(const DatabaseRow& row) {
    SessionRequest request;
    request.userId = row[0].as<std::string>();
    request.deviceId = row[1].as<std::string>();
    request.kind = static_cast<wire::SessionKind>(row[2].as<int>());
    request.writeControl = row[3].as<bool>();
    request.loginHash = row[4].as<std::string>("");
    request.grantRevision = row[5].as<std::int64_t>(-1);
    return request;
}
constexpr std::string_view kSessionFields = "SELECT user_id,device_id,kind,write_control,login_token_hash,grant_revision FROM remote_sessions WHERE id=?";
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
    DatabaseTransaction tx(connection);
    if (!permission(tx, request)) {
        auditDenied(tx, request, "permission_denied");
        return std::unexpected(SessionFailure::kDenied);
    }
    if (request.kind == wire::DESKTOP && request.writeControl && !request.agentInputContractSupported) {
        auditDenied(tx, request, "desktop_input_upgrade_required");
        return std::unexpected(SessionFailure::kIncompatible);
    }
    const auto grant = tx.exec("SELECT revision,require_approval FROM device_grants WHERE device_id=? AND user_id=?", request.deviceId, request.userId);
    if (grant[0][1].as<bool>() && !TelegramService::consumeApproval(tx, request, grant[0][0].as<std::int64_t>())) {
        auditDenied(tx, request, "approval_required");
        return std::unexpected(SessionFailure::kApprovalRequired);
    }
    expirePending(tx, request.deviceId);
    const auto count = tx.exec("SELECT count(*) FROM remote_sessions WHERE user_id=? AND device_id<>? AND (state='active' OR (state='pending' AND setup_expires_at>now(6)))", request.userId, request.deviceId);
    if (count[0][0].as<int>() >= 16) {
        auditDenied(tx, request, "session_limit");
        return std::unexpected(SessionFailure::kLimit);
    }
    if (!request.preempt && !tx.exec("SELECT id FROM remote_sessions WHERE device_id=? AND state<>'closed'", request.deviceId).empty()) {
        auditDenied(tx, request, "device_busy");
        return std::unexpected(SessionFailure::kBusy);
    }
    if (!request.writeControl && !tx.exec("SELECT 1 FROM remote_sessions s JOIN devices d ON d.id=s.device_id WHERE s.device_id=? AND s.user_id<>? AND d.owner_id<>? AND s.state<>'closed'", request.deviceId, request.userId, request.userId).empty()) {
        auditDenied(tx, request, "device_busy");
        return std::unexpected(SessionFailure::kBusy);
    }
    IssuedSession result{"s-" + randomToken(), {}, {}};
    const auto preempted = endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state<>'closed' FOR UPDATE", request.deviceId), "preempted");
    result.preemptedSessions.reserve(preempted.size());
    for (const auto& row : preempted) {
        result.preemptedSessions.push_back(row[0].as<std::string>());
    }
    tx.exec("INSERT INTO remote_sessions(id,user_id,device_id,kind,write_control,setup_expires_at,login_token_hash,grant_revision) VALUES(?,?,?,?,?,now(6)+INTERVAL 60 SECOND,?,?)", result.id, request.userId, request.deviceId, static_cast<int>(request.kind), request.writeControl, request.loginHash, grant[0][0].as<std::int64_t>());
    const std::array purposes{wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO, wire::VNC_STREAM, wire::SSH_STREAM};
    for (auto purpose : purposes) {
        if (!protocol::purposeMatches(request.kind, purpose)) {
            continue;
        }
        IssuedChannel channel{"c-" + randomToken(), purpose, randomToken(), randomToken()};
        tx.exec("INSERT INTO session_channels(id,session_id,purpose,browser_ticket_hash,agent_ticket_hash,expires_at) VALUES(?,?,?,?,?,now(6)+INTERVAL 60 SECOND)", channel.id, result.id, static_cast<int>(purpose), tokenHash(channel.browserTicket), tokenHash(channel.agentTicket));
        result.channels.push_back(std::move(channel));
    }
    tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES(?,?,?,'session.create',true)", request.userId, request.deviceId, result.id);
    tx.commit();
    return result;
}
std::optional<ChannelAuthorization> SessionService::consumeBrowser(const UserIdentity& user, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    if (user.id.empty() || !validToken(user.loginHash)) {
        return std::nullopt;
    }
    return consume(&user, nullptr, sessionId, channelId, purpose, ticket);
}
std::optional<ChannelAuthorization> SessionService::consumeAgent(const DeviceIdentity& device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    return consume(nullptr, &device, sessionId, channelId, purpose, ticket);
}
std::optional<ChannelAuthorization> SessionService::consume(const UserIdentity* user, const DeviceIdentity* device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const {
    if (!validToken(ticket) || sessionId.size() > 128 || channelId.size() > 128) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec(kSessionFields, sessionId);
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto request = requestFrom(rows[0]);
    if ((device ? device->id != request.deviceId : (!user || user->id != request.userId || user->loginHash != request.loginHash)) || !protocol::purposeMatches(request.kind, purpose) || !permission(tx, request)) {
        return std::nullopt;
    }
    if (device && tx.exec("SELECT id FROM devices WHERE id=? AND credential_hash=?", device->id, device->credentialHash).empty()) {
        return std::nullopt;
    }
    const auto session = tx.exec("SELECT id FROM remote_sessions WHERE id=? AND (state='active' OR (state='pending' AND setup_expires_at>now(6))) FOR UPDATE", sessionId);
    if (session.empty()) {
        return std::nullopt;
    }
    const auto query = device ? "UPDATE session_channels SET agent_consumed_at=now(6) WHERE id=? AND session_id=? AND purpose=? AND agent_ticket_hash=? AND agent_consumed_at IS NULL AND expires_at>now(6)" : "UPDATE session_channels SET browser_consumed_at=now(6) WHERE id=? AND session_id=? AND purpose=? AND browser_ticket_hash=? AND browser_consumed_at IS NULL AND expires_at>now(6)";
    if (tx.exec(query, channelId, sessionId, static_cast<int>(purpose), tokenHash(ticket)).affectedRows == 0) {
        return std::nullopt;
    }
    if (tx.exec("UPDATE remote_sessions SET state='active' WHERE id=? AND state='pending' AND NOT EXISTS (SELECT 1 FROM session_channels WHERE session_id=? AND (browser_consumed_at IS NULL OR agent_consumed_at IS NULL))", sessionId, sessionId).affectedRows) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES(?,?,?,'session.active',true)", request.userId, request.deviceId, sessionId);
    }
    tx.commit();
    return ChannelAuthorization{std::string(sessionId), std::string(channelId), request.userId, request.deviceId, request.kind, purpose, request.writeControl};
}
bool SessionService::close(std::string_view userId, std::string_view sessionId, SessionEnd reason) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec(kSessionFields, sessionId);
    if (rows.empty() || rows[0][0].as<std::string>() != userId) {
        return false;
    }
    tx.exec("SELECT id FROM devices WHERE id=? FOR UPDATE", rows[0][1].as<std::string>());
    const auto ended = tx.exec("SELECT device_id FROM remote_sessions WHERE id=? AND state<>'closed' FOR UPDATE", sessionId);
    tx.exec("UPDATE remote_sessions SET state='closed',ended_at=now(6),end_reason=? WHERE id=? AND state<>'closed'", endReason(reason), sessionId);
    if (!ended.empty()) {
        tx.exec("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,'session.end',?,?)", userId, ended[0][0].as<std::string>(), sessionId, reason == SessionEnd::kUser, endReason(reason));
    }
    tx.commit();
    return true;
}
bool SessionService::authorized(std::string_view sessionId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec(kSessionFields, sessionId);
    if (rows.empty() || !permission(tx, requestFrom(rows[0]))) {
        return false;
    }
    const bool allowed = !tx.exec("SELECT id FROM remote_sessions WHERE id=? AND (state='active' OR (state='pending' AND setup_expires_at>now(6)))", sessionId).empty();
    tx.commit();
    return allowed;
}
void SessionService::recoverAfterRestart() const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE state<>'closed' FOR UPDATE"), "gateway_restarted");
    tx.commit();
}
std::optional<SessionInfo> SessionService::get(std::string_view userId, std::string_view sessionId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT id,device_id,state,coalesce(end_reason,'') FROM remote_sessions WHERE id=? AND user_id=?", sessionId, userId);
    if (rows.empty()) {
        return std::nullopt;
    }
    return SessionInfo{rows[0][0].as<std::string>(), rows[0][1].as<std::string>(), rows[0][2].as<std::string>(), rows[0][3].as<std::string>()};
}
bool SessionService::closeForDevice(const DeviceIdentity& device, std::string_view sessionId, SessionEnd reason) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=? AND credential_hash=? FOR UPDATE", device.id, device.credentialHash).empty()) {
        return false;
    }
    const auto rows = tx.exec("SELECT id FROM remote_sessions WHERE id=? AND device_id=?", sessionId, device.id);
    if (rows.empty()) {
        return false;
    }
    endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE id=? AND device_id=? AND state<>'closed' FOR UPDATE", sessionId, device.id), endReason(reason), reason == SessionEnd::kAgentClosed);
    tx.commit();
    return true;
}
std::vector<std::string> SessionService::revokeDevice(std::string_view deviceId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state<>'closed' AND EXISTS (SELECT 1 FROM devices WHERE id=? AND revoked_at IS NOT NULL) FOR UPDATE", deviceId, deviceId), "device_deleted");
    std::vector<std::string> result;
    for (const auto& row : rows) {
        result.push_back(row[0].as<std::string>());
    }
    tx.commit();
    return result;
}
void SessionService::disconnectDevice(const DeviceIdentity& device) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=? AND credential_hash=? FOR UPDATE", device.id, device.credentialHash).empty()) {
        return;
    }
    endSessions(tx, tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state<>'closed' FOR UPDATE", device.id), "agent_disconnected");
    tx.commit();
}
std::vector<SessionInfo> SessionService::revalidate(std::string_view userId) const {
    struct Candidate {
        std::string id, userId, deviceId;
    };
    std::vector<Candidate> candidates;
    std::vector<SessionInfo> result;
    {
        auto connection = openDatabase(database_);
        DatabaseTransaction tx(connection);
        for (const auto& row : tx.exec("SELECT id,user_id,device_id FROM remote_sessions WHERE state<>'closed' AND (?='' OR user_id=?)", userId, userId)) {
            candidates.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>()});
        }
    }
    for (auto& item : candidates) {
        if (!authorized(item.id) && close(item.userId, item.id, SessionEnd::kPermissionRevoked)) {
            result.push_back({item.id, item.deviceId, "closed", "permission_revoked"});
        }
    }
    return result;
}
} // namespace ultraterimal::server
