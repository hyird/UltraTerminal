#include "session.service.h"

#include <array>

#include "common/database.h"
#include "modules/devices/device.entity.h"
#include "modules/identity/identity.service.h"
#include "modules/telegram/telegram.service.h"
#include "session.entity.h"

namespace ultraterimal::server {
namespace {
bool validRequest(const SessionRequest& request) {
    if (request.userId.empty() || request.userId.size() > 128 || request.deviceId.empty() || request.deviceId.size() > 128 ||
        request.kind < wire::DESKTOP || request.kind > wire::SSH || !request.writeControl || request.reconnectSessionId.size() > 128 ||
        (!request.reconnectSessionId.empty() && (request.preempt || request.kind != wire::DESKTOP))) {
        return false;
    }
    if (request.kind == wire::DESKTOP) {
        if (request.desktopInputContract != 2) {
            return false;
        }
    } else if (request.desktopInputContract) {
        return false;
    }
    return true;
}
ruvia::task<bool> permission(ruvia::db_transaction& tx, const SessionRequest& request) {
    if (!request.writeControl || !validToken(request.loginHash)) {
        co_return false;
    }
    // Device, user, grant, session is the lock order for all session transactions.
    const auto deviceRows = co_await tx.query("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", request.deviceId);
    if (deviceRows.empty()) {
        co_return false;
    }
    const auto userRows = co_await tx.query("SELECT id FROM users WHERE id=? AND NOT disabled FOR UPDATE", request.userId);
    if (userRows.empty()) {
        co_return false;
    }
    const auto grants = co_await tx.query("SELECT desktop,vnc,ssh,write_control,revision FROM device_grants WHERE device_id=? AND user_id=? LOCK IN SHARE MODE", request.deviceId, request.userId);
    if (grants.empty()) {
        co_return false;
    }
    const auto loginRows = co_await tx.query("SELECT 1 FROM login_sessions WHERE token_hash=? AND user_id=? AND expires_at>UTC_TIMESTAMP(6)", request.loginHash, request.userId);
    co_return !loginRows.empty() && (!request.grantRevision || request.grantRevision == dbField<std::int64_t>(grants[0][4])) && dbField<bool>(grants[0][static_cast<int>(request.kind) - 1]) && (!request.writeControl || dbField<bool>(grants[0][3]));
}
struct EndedSessionRow {
    std::string id;
    std::string userId;
    std::string deviceId;
};
ruvia::task<std::vector<EndedSessionRow>> endSessions(ruvia::db_transaction& tx, ruvia::db_rows rows, std::string reason, bool success = false) {
    std::vector<EndedSessionRow> ended;
    ended.reserve(rows.size());
    for (const auto& row : rows) {
        ended.push_back({dbField<std::string>(row[0]), dbField<std::string>(row[1]), dbField<std::string>(row[2])});
    }
    for (const auto& item : ended) {
        co_await tx.execute("UPDATE remote_sessions SET state='closed',ended_at=UTC_TIMESTAMP(6),end_reason=? WHERE id=?", reason, item.id);
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,'session.end',?,?)", item.userId, item.deviceId, item.id, success, reason);
    }
    co_return ended;
}
ruvia::task<void> expirePending(ruvia::db_transaction& tx, std::string deviceId) {
    auto rows = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state='pending' AND setup_expires_at<=UTC_TIMESTAMP(6) FOR UPDATE", deviceId);
    (void)co_await endSessions(tx, std::move(rows), "setup_expired");
}
ruvia::task<void> auditDenied(ruvia::db_transaction& tx, const SessionRequest& request, std::string reason) {
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) SELECT u.id,d.id,'session.create',false,? FROM users u LEFT JOIN devices d ON d.id=? WHERE u.id=?", reason, request.deviceId, request.userId);
    co_await tx.commit();
}
SessionRequest requestFrom(const ruvia::db_row& row) {
    SessionRequest request;
    request.userId = dbField<std::string>(row[0]);
    request.deviceId = dbField<std::string>(row[1]);
    request.kind = static_cast<wire::SessionKind>(dbField<int>(row[2]));
    request.writeControl = dbField<bool>(row[3]);
    request.loginHash = dbFieldOr<std::string>(row[4], "");
    request.grantRevision = dbFieldOr<std::int64_t>(row[5], -1);
    return request;
}
constexpr std::string_view kSessionFields = "SELECT user_id,device_id,kind,write_control,login_token_hash,grant_revision FROM remote_sessions WHERE id=?";
std::string endReason(SessionEnd reason) {
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
ruvia::task<std::expected<IssuedSession, SessionFailure>> SessionService::create(ruvia::db_executor db, SessionRequest request) const {
    if (!validRequest(request)) {
        co_return std::unexpected(SessionFailure::kInvalid);
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    if (!(co_await permission(tx, request))) {
        co_await auditDenied(tx, request, "permission_denied");
        co_return std::unexpected(SessionFailure::kDenied);
    }
    if (request.kind == wire::DESKTOP && request.writeControl && !request.agentInputContractSupported) {
        co_await auditDenied(tx, request, "desktop_input_upgrade_required");
        co_return std::unexpected(SessionFailure::kIncompatible);
    }
    const auto grant = co_await tx.query("SELECT revision,verification_method FROM device_grants WHERE device_id=? AND user_id=?", request.deviceId, request.userId);
    const auto grantRevision = dbField<std::int64_t>(grant[0][0]);
    const auto method = dbField<std::string>(grant[0][1]);
    co_await expirePending(tx, request.deviceId);
    const auto count = co_await tx.query("SELECT count(*) FROM remote_sessions WHERE user_id=? AND device_id<>? AND (state='active' OR (state='pending' AND setup_expires_at>UTC_TIMESTAMP(6)))", request.userId, request.deviceId);
    if (dbField<int>(count[0][0]) >= 16) {
        co_await auditDenied(tx, request, "session_limit");
        co_return std::unexpected(SessionFailure::kLimit);
    }
    if (request.kind != wire::SSH && !request.preempt) {
        if (co_await tx.repository<RemoteSessionEntity>().exists({.where_ = RemoteSessionEntity::column<"device_id">() == request.deviceId && RemoteSessionEntity::column<"kind">() != static_cast<int>(wire::SSH) && RemoteSessionEntity::column<"state">() != "closed"})) {
            co_await auditDenied(tx, request, "device_busy");
            co_return std::unexpected(SessionFailure::kBusy);
        }
    }
    std::optional<std::string> reconnectUntil;
    if (!request.reconnectSessionId.empty()) {
        const auto reconnect = co_await tx.query("SELECT DATE_FORMAT(COALESCE(reconnect_until,ended_at+INTERVAL 5 MINUTE),'%Y-%m-%d %H:%i:%s.%f') FROM remote_sessions WHERE id=? AND user_id=? AND device_id=? AND login_token_hash=? AND grant_revision=? AND kind=? AND write_control=? AND state='closed' AND end_reason IN ('session_failed','agent_disconnected','gateway_restarted','setup_expired') AND reconnected_as IS NULL AND COALESCE(reconnect_until,ended_at+INTERVAL 5 MINUTE)>=UTC_TIMESTAMP(6) FOR UPDATE", request.reconnectSessionId, request.userId, request.deviceId, request.loginHash, grantRevision, static_cast<int>(request.kind), request.writeControl);
        if (!reconnect.empty()) {
            reconnectUntil = dbField<std::string>(reconnect[0][0]);
        }
    }
    const bool resumed = reconnectUntil.has_value();
    if (!resumed && method == "telegram" && !(co_await TelegramService::consumeApproval(tx, request, grantRevision))) {
        co_await auditDenied(tx, request, "approval_required");
        co_return std::unexpected(SessionFailure::kApprovalRequired);
    }
    if (!resumed && method == "totp") {
        if (request.verificationCode.empty()) {
            co_return std::unexpected(SessionFailure::kVerificationRequired);
        }
        if (!(co_await IdentityService(key_).verifyConnectionFactor(tx, UserIdentity{request.userId, {}, "user", request.loginHash}, request.verificationCode))) {
            co_await auditDenied(tx, request, "verification_failed");
            co_return std::unexpected(SessionFailure::kVerificationFailed);
        }
    }
    IssuedSession result{"s-" + randomToken(), {}, {}};
    if (request.kind != wire::SSH) {
        auto toPreempt = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND kind<>? AND state<>'closed' FOR UPDATE", request.deviceId, static_cast<int>(wire::SSH));
        const auto preempted = co_await endSessions(tx, std::move(toPreempt), "preempted");
        result.preemptedSessions.reserve(preempted.size());
        for (const auto& item : preempted) {
            result.preemptedSessions.push_back(item.id);
        }
    }
    co_await tx.execute("INSERT INTO remote_sessions(id,user_id,device_id,kind,write_control,setup_expires_at,login_token_hash,grant_revision) VALUES(?,?,?,?,?,UTC_TIMESTAMP(6)+INTERVAL 60 SECOND,?,?)", result.id, request.userId, request.deviceId, static_cast<int>(request.kind), request.writeControl, request.loginHash, grantRevision);
    if (resumed) {
        co_await tx.execute("UPDATE remote_sessions SET reconnect_until=? WHERE id=?", *reconnectUntil, result.id);
        co_await tx.execute("UPDATE remote_sessions SET reconnected_as=? WHERE id=?", result.id, request.reconnectSessionId);
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,'session.reconnect',true,?)", request.userId, request.deviceId, result.id, request.reconnectSessionId);
    }
    const std::array purposes{wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO, wire::VNC_STREAM, wire::SSH_STREAM};
    for (auto purpose : purposes) {
        if (!protocol::purposeMatches(request.kind, purpose)) {
            continue;
        }
        IssuedChannel channel{"c-" + randomToken(), purpose, randomToken(), randomToken()};
        co_await tx.execute("INSERT INTO session_channels(id,session_id,purpose,browser_ticket_hash,agent_ticket_hash,expires_at) VALUES(?,?,?,?,?,UTC_TIMESTAMP(6)+INTERVAL 60 SECOND)", channel.id, result.id, static_cast<int>(purpose), tokenHash(channel.browserTicket), tokenHash(channel.agentTicket));
        result.channels.push_back(std::move(channel));
    }
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES(?,?,?,'session.create',true)", request.userId, request.deviceId, result.id);
    co_await tx.commit();
    co_return result;
}
ruvia::task<std::optional<ChannelAuthorization>> SessionService::consumeBrowser(ruvia::db_executor db, UserIdentity user, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const {
    if (user.id.empty() || !validToken(user.loginHash)) {
        co_return std::nullopt;
    }
    co_return co_await consume(db, std::move(user), std::nullopt, std::move(sessionId), std::move(channelId), purpose, std::move(ticket));
}
ruvia::task<std::optional<ChannelAuthorization>> SessionService::consumeAgent(ruvia::db_executor db, DeviceIdentity device, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const {
    co_return co_await consume(db, std::nullopt, std::move(device), std::move(sessionId), std::move(channelId), purpose, std::move(ticket));
}
ruvia::task<std::optional<ChannelAuthorization>> SessionService::consume(ruvia::db_executor db, std::optional<UserIdentity> user, std::optional<DeviceIdentity> device, std::string sessionId, std::string channelId, wire::ChannelPurpose purpose, std::string ticket) const {
    if (!validToken(ticket) || sessionId.size() > 128 || channelId.size() > 128) {
        co_return std::nullopt;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query(kSessionFields, sessionId);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto request = requestFrom(rows[0]);
    if ((device ? device->id != request.deviceId : (!user || user->id != request.userId || user->loginHash != request.loginHash)) || !protocol::purposeMatches(request.kind, purpose) || !(co_await permission(tx, request))) {
        co_return std::nullopt;
    }
    if (device) {
        if (!(co_await tx.repository<DeviceEntity>().exists({.where_ = DeviceEntity::column<"id">() == device->id && DeviceEntity::column<"credential_hash">() == device->credentialHash}))) {
            co_return std::nullopt;
        }
    }
    const auto session = co_await tx.query("SELECT id FROM remote_sessions WHERE id=? AND (state='active' OR (state='pending' AND setup_expires_at>UTC_TIMESTAMP(6))) FOR UPDATE", sessionId);
    if (session.empty()) {
        co_return std::nullopt;
    }
    const auto query = device ? "UPDATE session_channels SET agent_consumed_at=UTC_TIMESTAMP(6) WHERE id=? AND session_id=? AND purpose=? AND agent_ticket_hash=? AND agent_consumed_at IS NULL AND expires_at>UTC_TIMESTAMP(6)" : "UPDATE session_channels SET browser_consumed_at=UTC_TIMESTAMP(6) WHERE id=? AND session_id=? AND purpose=? AND browser_ticket_hash=? AND browser_consumed_at IS NULL AND expires_at>UTC_TIMESTAMP(6)";
    const auto consumed = co_await tx.execute(query, channelId, sessionId, static_cast<int>(purpose), tokenHash(ticket));
    if (consumed.affected_rows() == 0) {
        co_return std::nullopt;
    }
    const auto activated = co_await tx.execute("UPDATE remote_sessions SET state='active',reconnect_until=NULL WHERE id=? AND state='pending' AND NOT EXISTS (SELECT 1 FROM session_channels WHERE session_id=? AND (browser_consumed_at IS NULL OR agent_consumed_at IS NULL))", sessionId, sessionId);
    if (activated.affected_rows() > 0) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES(?,?,?,'session.active',true)", request.userId, request.deviceId, sessionId);
    }
    co_await tx.commit();
    co_return ChannelAuthorization{sessionId, channelId, request.userId, request.deviceId, request.kind, purpose, request.writeControl};
}
ruvia::task<bool> SessionService::close(ruvia::db_executor db, std::string userId, std::string sessionId, SessionEnd reason) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query(kSessionFields, sessionId);
    if (rows.empty() || dbField<std::string>(rows[0][0]) != userId) {
        co_return false;
    }
    (void)co_await tx.query("SELECT id FROM devices WHERE id=? FOR UPDATE", dbField<std::string>(rows[0][1]));
    const auto ended = co_await tx.query("SELECT device_id FROM remote_sessions WHERE id=? AND state<>'closed' FOR UPDATE", sessionId);
    const auto reasonText = endReason(reason);
    co_await tx.execute("UPDATE remote_sessions SET state='closed',ended_at=UTC_TIMESTAMP(6),end_reason=? WHERE id=? AND state<>'closed'", reasonText, sessionId);
    if (!ended.empty()) {
        co_await tx.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,'session.end',?,?)", userId, dbField<std::string>(ended[0][0]), sessionId, reason == SessionEnd::kUser, reasonText);
    }
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> SessionService::authorized(ruvia::db_executor db, std::string sessionId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto rows = co_await tx.query(kSessionFields, sessionId);
    if (rows.empty() || !(co_await permission(tx, requestFrom(rows[0])))) {
        co_return false;
    }
    const auto active = co_await tx.query("SELECT id FROM remote_sessions WHERE id=? AND (state='active' OR (state='pending' AND setup_expires_at>UTC_TIMESTAMP(6)))", sessionId);
    co_await tx.commit();
    co_return !active.empty();
}
ruvia::task<bool> SessionService::authorizeDesktopFileTransfer(ruvia::db_executor db, UserIdentity user, std::string deviceId, std::string sessionId, bool requireWrite) const {
    if (deviceId.empty() || deviceId.size() > 128 || sessionId.empty() || sessionId.size() > 128 || !validToken(user.loginHash)) {
        co_return false;
    }
    const auto rows = co_await db.query(
        "SELECT 1 FROM remote_sessions s "
        "JOIN devices d ON d.id=s.device_id AND d.revoked_at IS NULL "
        "JOIN users u ON u.id=s.user_id AND NOT u.disabled "
        "JOIN device_grants g ON g.device_id=s.device_id AND g.user_id=s.user_id AND g.revision=s.grant_revision "
        "JOIN login_sessions l ON l.token_hash=s.login_token_hash AND l.user_id=s.user_id AND l.expires_at>UTC_TIMESTAMP(6) "
        "WHERE s.id=? AND s.user_id=? AND s.device_id=? AND s.login_token_hash=? "
        "AND ((s.kind=? AND g.desktop=true) OR (s.kind=? AND g.ssh=true)) "
        "AND (s.state='active' OR (s.state='pending' AND s.setup_expires_at>UTC_TIMESTAMP(6))) "
        "AND (NOT ? OR (s.write_control=true AND g.write_control=true))",
        sessionId, user.id, deviceId, user.loginHash, static_cast<int>(wire::DESKTOP), static_cast<int>(wire::SSH), requireWrite);
    co_return !rows.empty();
}
ruvia::task<void> SessionService::auditFileTransfer(ruvia::db_executor db, std::string userId, std::string deviceId, std::string sessionId, std::string eventType, bool success, std::string reason) const {
    try {
        if (reason.empty()) {
            co_await db.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success) VALUES(?,?,?,?,?)", userId, deviceId, sessionId, eventType, success);
        } else {
            co_await db.execute("INSERT INTO audit_events(user_id,device_id,session_id,event_type,success,reason) VALUES(?,?,?,?,?,?)", userId, deviceId, sessionId, eventType, success, reason);
        }
    } catch (...) {
    }
}
ruvia::task<void> SessionService::recoverAfterRestart(ruvia::db_executor db) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    auto rows = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE state<>'closed' FOR UPDATE");
    (void)co_await endSessions(tx, std::move(rows), "gateway_restarted");
    co_await tx.commit();
}
ruvia::task<std::optional<SessionInfo>> SessionService::get(ruvia::db_executor db, std::string userId, std::string sessionId) const {
    const auto found = co_await db.repository<RemoteSessionEntity>().find_first({.where_ = RemoteSessionEntity::column<"id">() == sessionId && RemoteSessionEntity::column<"user_id">() == userId});
    if (!found) {
        co_return std::nullopt;
    }
    co_return SessionInfo{found->get<"id">(), found->get<"device_id">(), found->get<"state">(), found->is_null<"end_reason">() ? std::string{} : found->get<"end_reason">()};
}
ruvia::task<bool> SessionService::closeForDevice(ruvia::db_executor db, DeviceIdentity device, std::string sessionId, SessionEnd reason) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto devRows = co_await tx.query("SELECT id FROM devices WHERE id=? AND credential_hash=? FOR UPDATE", device.id, device.credentialHash);
    if (devRows.empty()) {
        co_return false;
    }
    if (!(co_await tx.repository<RemoteSessionEntity>().exists({.where_ = RemoteSessionEntity::column<"id">() == sessionId && RemoteSessionEntity::column<"device_id">() == device.id}))) {
        co_return false;
    }
    auto toClose = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE id=? AND device_id=? AND state<>'closed' FOR UPDATE", sessionId, device.id);
    (void)co_await endSessions(tx, std::move(toClose), endReason(reason), reason == SessionEnd::kAgentClosed);
    co_await tx.commit();
    co_return true;
}
ruvia::task<std::vector<std::string>> SessionService::revokeDevice(ruvia::db_executor db, std::string deviceId) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    auto toRevoke = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state<>'closed' AND EXISTS (SELECT 1 FROM devices WHERE id=? AND revoked_at IS NOT NULL) FOR UPDATE", deviceId, deviceId);
    const auto ended = co_await endSessions(tx, std::move(toRevoke), "device_deleted");
    std::vector<std::string> result;
    result.reserve(ended.size());
    for (const auto& item : ended) {
        result.push_back(item.id);
    }
    co_await tx.commit();
    co_return result;
}
ruvia::task<void> SessionService::disconnectDevice(ruvia::db_executor db, DeviceIdentity device) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto devRows = co_await tx.query("SELECT id FROM devices WHERE id=? AND credential_hash=? FOR UPDATE", device.id, device.credentialHash);
    if (devRows.empty()) {
        co_return;
    }
    auto toClose = co_await tx.query("SELECT id,user_id,device_id FROM remote_sessions WHERE device_id=? AND state<>'closed' FOR UPDATE", device.id);
    (void)co_await endSessions(tx, std::move(toClose), "agent_disconnected");
    co_await tx.commit();
}
ruvia::task<std::vector<SessionInfo>> SessionService::revalidate(ruvia::db_executor db, std::string userId) const {
    struct Candidate {
        std::string id, userId, deviceId;
    };
    std::vector<Candidate> candidates;
    std::vector<SessionInfo> result;
    ruvia::db_find_options options;
    options.where_ = RemoteSessionEntity::column<"state">() != "closed";
    if (!userId.empty()) {
        options.where_ = std::move(options.where_) && RemoteSessionEntity::column<"user_id">() == userId;
    }
    const auto rows = co_await db.repository<RemoteSessionEntity>().find(options);
    for (const auto& row : rows) {
        candidates.push_back({row.get<"id">(), row.get<"user_id">(), row.get<"device_id">()});
    }
    for (auto& item : candidates) {
        if (!(co_await authorized(db, item.id)) && co_await close(db, item.userId, item.id, SessionEnd::kPermissionRevoked)) {
            result.push_back({item.id, item.deviceId, "closed", "permission_revoked"});
        }
    }
    co_return result;
}
} // namespace ultraterimal::server
