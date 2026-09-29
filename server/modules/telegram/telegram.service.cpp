#include "telegram.service.h"

#include "modules/identity/security.protocol.h"

namespace ultraterimal::server {
namespace {
bool activeUser(DatabaseTransaction& tx, const UserIdentity& user, bool admin = false) {
    return !tx.exec("SELECT u.id FROM users u JOIN login_sessions s ON s.user_id=u.id WHERE u.id=? AND s.token_hash=? AND s.expires_at>now(6) AND NOT u.disabled AND (NOT ? OR u.role='admin') FOR UPDATE", user.id, user.loginHash, admin).empty();
}
// All approvals are tied to current bindings, owner, grant revision and login.
constexpr std::string_view kValidApproval =
    " SELECT a.id FROM connection_approvals a JOIN devices d ON d.id=a.device_id AND d.owner_id=a.owner_id AND d.revoked_at IS NULL"
    " JOIN device_grants g ON g.device_id=a.device_id AND g.user_id=a.user_id AND g.revision=a.grant_revision AND g.desktop AND (NOT a.write_control OR g.write_control) AND g.require_approval"
    " JOIN users u ON u.id=a.user_id AND NOT u.disabled JOIN users o ON o.id=a.owner_id AND NOT o.disabled"
    " JOIN login_sessions l ON l.token_hash=a.login_token_hash AND l.user_id=a.user_id AND l.expires_at>now(6)"
    " JOIN telegram_accounts r ON r.user_id=a.user_id AND r.telegram_id=a.requester_telegram_id"
    " JOIN telegram_accounts t ON t.user_id=a.owner_id AND t.telegram_id=a.owner_telegram_id"
    " JOIN telegram_settings b ON b.generation=a.bot_generation WHERE a.expires_at>now(6)";
} // namespace
TelegramState TelegramService::state(std::string_view userId) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    TelegramState result;
    const auto bot = tx.exec("SELECT bot_username FROM telegram_settings");
    if (!bot.empty()) {
        result.botUsername = bot[0][0].as<std::string>();
    }
    const auto account = tx.exec("SELECT telegram_id FROM telegram_accounts WHERE user_id=?", userId);
    if (!account.empty()) {
        result.telegramId = account[0][0].as<std::string>();
    }
    return result;
}
std::optional<TelegramConfig> TelegramService::configuration() const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto rows = tx.exec("SELECT token,bot_username,webhook_secret,generation FROM telegram_settings");
    if (rows.empty()) {
        return std::nullopt;
    }
    return TelegramConfig{openSecret(key_, "telegram:token", rows[0][0].as<std::string>()), rows[0][1].as<std::string>(), openSecret(key_, "telegram:webhook", rows[0][2].as<std::string>()), rows[0][3].as<std::string>()};
}
bool TelegramService::configure(const UserIdentity& actor, const TelegramConfig& config) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!activeUser(tx, actor, true)) {
        return false;
    }
    tx.exec("DELETE FROM telegram_bindings");
    tx.exec("DELETE FROM telegram_accounts");
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE state IN ('pending','approved')");
    tx.exec("INSERT INTO telegram_settings(singleton,token,bot_username,webhook_secret,generation) VALUES(true,?,?,?,?) ON DUPLICATE KEY UPDATE token=?,bot_username=?,webhook_secret=?,generation=?", sealSecret(key_, "telegram:token", config.token), config.username, sealSecret(key_, "telegram:webhook", config.webhookSecret), config.generation, sealSecret(key_, "telegram:token", config.token), config.username, sealSecret(key_, "telegram:webhook", config.webhookSecret), config.generation);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.configure',true)", actor.id);
    tx.commit();
    return true;
}
bool TelegramService::disable(const UserIdentity& actor) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!activeUser(tx, actor, true)) {
        return false;
    }
    tx.exec("DELETE FROM telegram_settings");
    tx.exec("DELETE FROM telegram_bindings");
    tx.exec("DELETE FROM telegram_accounts");
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE state IN ('pending','approved')");
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.disable',true)", actor.id);
    tx.commit();
    return true;
}
std::optional<std::string> TelegramService::beginBinding(const UserIdentity& user) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!activeUser(tx, user) || tx.exec("SELECT 1 FROM telegram_accounts WHERE user_id=?", user.id).size()) {
        return std::nullopt;
    }
    const auto bot = tx.exec("SELECT bot_username FROM telegram_settings");
    if (bot.empty()) {
        return std::nullopt;
    }
    const auto token = randomToken();
    tx.exec("DELETE FROM telegram_bindings WHERE user_id=? OR expires_at<=now(6)", user.id);
    tx.exec("INSERT INTO telegram_bindings(token_hash,user_id,login_token_hash,expires_at) VALUES(?,?,?,now(6)+INTERVAL 10 MINUTE)", tokenHash(token), user.id, user.loginHash);
    tx.commit();
    return "https://t.me/" + bot[0][0].as<std::string>() + "?start=" + token;
}
std::optional<std::string> TelegramService::bind(std::string_view generation, std::string_view token, std::int64_t telegramId) const {
    if (!validToken(token) || telegramId <= 0) {
        return std::nullopt;
    }
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (tx.exec("SELECT 1 FROM telegram_settings WHERE generation=?", generation).empty()) {
        return std::nullopt;
    }
    const auto rows = tx.exec("SELECT b.user_id FROM telegram_bindings b JOIN users u ON u.id=b.user_id JOIN login_sessions s ON s.token_hash=b.login_token_hash AND s.user_id=b.user_id WHERE b.token_hash=? AND b.expires_at>now(6) AND s.expires_at>now(6) AND NOT u.disabled FOR UPDATE", tokenHash(token));
    if (rows.empty()) {
        return std::nullopt;
    }
    const auto user = rows[0][0].as<std::string>();
    if (!tx.exec("SELECT user_id FROM telegram_accounts WHERE user_id=? OR telegram_id=?", user, telegramId).empty()) {
        return std::nullopt;
    }
    tx.exec("INSERT INTO telegram_accounts(user_id,telegram_id) VALUES(?,?)", user, telegramId);
    tx.exec("DELETE FROM telegram_bindings WHERE user_id=?", user);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.bind',true)", user);
    tx.commit();
    return user;
}
void TelegramService::unbind(const UserIdentity& user) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!activeUser(tx, user)) {
        return;
    }
    tx.exec("DELETE FROM telegram_accounts WHERE user_id=?", user.id);
    tx.exec("DELETE FROM telegram_bindings WHERE user_id=?", user.id);
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE (user_id=? OR owner_id=?) AND state IN ('pending','approved')", user.id, user.id);
    tx.exec("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.unbind',true)", user.id);
    tx.commit();
}
std::optional<ApprovalDispatch> TelegramService::requestApproval(const UserIdentity& user, std::string_view deviceId, bool writeControl) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    if (tx.exec("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId).empty() || !activeUser(tx, user)) {
        return std::nullopt;
    }
    const auto rows = tx.exec("SELECT d.owner_id,d.name,g.revision,u.username,r.telegram_id,t.telegram_id,b.token,b.generation FROM devices d JOIN device_grants g ON g.device_id=d.id AND g.user_id=? JOIN users u ON u.id=g.user_id JOIN users o ON o.id=d.owner_id AND NOT o.disabled JOIN telegram_accounts r ON r.user_id=u.id JOIN telegram_accounts t ON t.user_id=d.owner_id CROSS JOIN telegram_settings b WHERE d.id=? AND g.desktop AND (NOT ? OR g.write_control) AND g.require_approval", user.id, deviceId, writeControl);
    if (rows.empty()) {
        return std::nullopt;
    }
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE user_id=? AND state IN ('pending','approved') AND expires_at<=now(6)", user.id);
    tx.exec("DELETE FROM connection_approvals WHERE created_at<now(6)-INTERVAL 7 DAY");
    // Bound notification volume per requester, including retries after cancellation.
    if (tx.exec("SELECT count(*) FROM connection_approvals WHERE user_id=? AND created_at>now(6)-INTERVAL 1 MINUTE", user.id)[0][0].as<int>() >= 5) {
        return std::nullopt;
    }
    const auto existing = tx.exec("SELECT id,write_control,FLOOR(UNIX_TIMESTAMP(expires_at)) FROM connection_approvals WHERE user_id=? AND device_id=? AND login_token_hash=? AND state='pending'", user.id, deviceId, user.loginHash);
    if (!existing.empty()) {
        ApprovalDispatch result{{existing[0][0].as<std::string>(), std::string(deviceId), "pending", existing[0][1].as<bool>(), existing[0][2].as<std::int64_t>()}, {}, {}, {}, 0};
        tx.commit();
        return result;
    }
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE user_id=? AND device_id=? AND state IN ('pending','approved')", user.id, deviceId);
    const auto id = randomToken().substr(0, 32);
    const auto inserted = tx.exec("INSERT INTO connection_approvals(id,user_id,device_id,login_token_hash,write_control,grant_revision,requester_telegram_id,owner_id,owner_telegram_id,bot_generation,state,expires_at) VALUES(?,?,?,?,?,?,?,?,?,?,'pending',now(6)+INTERVAL 5 MINUTE) RETURNING FLOOR(UNIX_TIMESTAMP(expires_at))", id, user.id, deviceId, user.loginHash, writeControl, rows[0][2].as<std::int64_t>(), rows[0][4].as<std::int64_t>(), rows[0][0].as<std::string>(), rows[0][5].as<std::int64_t>(), rows[0][7].as<std::string>());
    ApprovalDispatch result{{id, std::string(deviceId), "pending", writeControl, inserted[0][0].as<std::int64_t>()}, openSecret(key_, "telegram:token", rows[0][6].as<std::string>()), rows[0][3].as<std::string>(), rows[0][1].as<std::string>(), rows[0][5].as<std::int64_t>()};
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'connection.request',true,?)", user.id, deviceId, id);
    tx.commit();
    return result;
}
void TelegramService::deliveryFailed(std::string_view id) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("UPDATE connection_approvals SET state='failed' WHERE id=? AND state='pending'", id);
    tx.commit();
}
std::optional<std::string> TelegramService::decide(std::string_view generation, std::string_view id, std::int64_t telegramId, bool accept) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    const auto changed = tx.exec("UPDATE connection_approvals SET state=?,decided_at=now(6) WHERE id=? AND owner_telegram_id=? AND bot_generation=? AND state='pending' AND id IN (" + std::string(kValidApproval) + ")", accept ? "approved" : "denied", id, telegramId, generation);
    if (!changed.affectedRows) {
        return std::nullopt;
    }
    const auto rows = tx.exec("SELECT user_id,device_id,owner_id FROM connection_approvals WHERE id=?", id);
    tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'connection.decide',?,?)", rows[0][2].as<std::string>(), rows[0][1].as<std::string>(), accept, id);
    tx.commit();
    return rows[0][0].as<std::string>();
}
std::vector<ApprovalInfo> TelegramService::approvals(const UserIdentity& user) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    std::vector<ApprovalInfo> result;
    for (const auto& row : tx.exec("SELECT id,device_id,CASE WHEN state IN ('pending','approved') AND (expires_at<=now(6) OR id NOT IN (" + std::string(kValidApproval) + ")) THEN 'cancelled' ELSE state END,write_control,FLOOR(UNIX_TIMESTAMP(expires_at)) FROM connection_approvals WHERE user_id=? AND login_token_hash=? ORDER BY created_at DESC LIMIT 20", user.id, user.loginHash)) {
        result.push_back({row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(), row[3].as<bool>(), row[4].as<std::int64_t>()});
    }
    return result;
}
void TelegramService::cancel(const UserIdentity& user, std::string_view id) const {
    auto connection = openDatabase(database_);
    DatabaseTransaction tx(connection);
    tx.exec("UPDATE connection_approvals SET state='cancelled' WHERE id=? AND user_id=? AND login_token_hash=? AND state IN ('pending','approved')", id, user.id, user.loginHash);
    tx.commit();
}
bool TelegramService::consumeApproval(DatabaseTransaction& tx, const SessionRequest& request, std::int64_t revision) {
    if (request.kind != wire::DESKTOP || request.approvalId.size() != 32) {
        return false;
    }
    return tx.exec("UPDATE connection_approvals SET state='consumed' WHERE id=? AND user_id=? AND device_id=? AND login_token_hash=? AND write_control=? AND grant_revision=? AND state='approved' AND id IN (" + std::string(kValidApproval) + ")", request.approvalId, request.userId, request.deviceId, request.loginHash, request.writeControl, revision).affectedRows != 0;
}
} // namespace ultraterimal::server
