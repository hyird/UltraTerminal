#include "telegram.service.h"

#include "common/database.h"
#include "modules/identity/identity.entity.h"
#include "modules/identity/security.protocol.h"
#include "telegram.entity.h"

namespace ultraterimal::server {
namespace {
ruvia::task<bool> activeUser(ruvia::db_transaction& tx, const UserIdentity& user, bool admin = false) {
    const auto rows = co_await tx.query("SELECT u.id FROM users u JOIN login_sessions s ON s.user_id=u.id WHERE u.id=? AND s.token_hash=? AND s.expires_at>UTC_TIMESTAMP(6) AND NOT u.disabled AND (NOT ? OR u.role='admin') FOR UPDATE", user.id, user.loginHash, admin);
    co_return !rows.empty();
}
// Connection confirmations are tied to the requester binding, grant revision and login.
constexpr std::string_view kValidApproval =
    " SELECT a.id FROM connection_verifications a JOIN devices d ON d.id=a.device_id AND d.revoked_at IS NULL"
    " JOIN device_grants g ON g.device_id=a.device_id AND g.user_id=a.user_id AND g.revision=a.grant_revision AND g.desktop AND a.write_control AND g.write_control AND g.verification_method='telegram'"
    " JOIN users u ON u.id=a.user_id AND NOT u.disabled"
    " JOIN login_sessions l ON l.token_hash=a.login_token_hash AND l.user_id=a.user_id AND l.expires_at>UTC_TIMESTAMP(6)"
    " JOIN telegram_accounts r ON r.user_id=a.user_id AND r.telegram_id=a.requester_telegram_id"
    " JOIN telegram_settings b ON b.generation=a.bot_generation WHERE a.expires_at>UTC_TIMESTAMP(6)";
} // namespace
ruvia::task<TelegramState> TelegramService::state(ruvia::db_executor db, std::string userId) const {
    TelegramState result;
    if (const auto bot = co_await db.repository<TelegramSettingsEntity>().find_first({})) {
        result.botUsername = bot->get<"bot_username">();
    }
    if (const auto account = co_await db.repository<TelegramAccountEntity>().find_first({.where_ = TelegramAccountEntity::column<"user_id">() == userId})) {
        result.telegramId = std::to_string(account->get<"telegram_id">());
    }
    co_return result;
}
ruvia::task<std::optional<TelegramConfig>> TelegramService::configuration(ruvia::db_executor db) const {
    const auto bot = co_await db.repository<TelegramSettingsEntity>().find_first({});
    if (!bot) {
        co_return std::nullopt;
    }
    co_return TelegramConfig{openSecret(key_, "telegram:token", bot->get<"token">()), bot->get<"bot_username">(), openSecret(key_, "telegram:webhook", bot->get<"webhook_secret">()), bot->get<"generation">()};
}
ruvia::task<bool> TelegramService::configure(ruvia::db_executor db, UserIdentity actor, TelegramConfig config) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!(co_await activeUser(tx, actor, true))) {
        co_return false;
    }
    co_await tx.execute("DELETE FROM telegram_bindings");
    co_await tx.execute("DELETE FROM identity_telegram_verifications");
    co_await tx.execute("DELETE FROM telegram_accounts");
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE state IN ('pending','approved')");
    const auto sealedToken = sealSecret(key_, "telegram:token", config.token);
    const auto sealedWebhook = sealSecret(key_, "telegram:webhook", config.webhookSecret);
    co_await tx.execute("INSERT INTO telegram_settings(singleton,token,bot_username,webhook_secret,generation) VALUES(true,?,?,?,?) ON DUPLICATE KEY UPDATE token=?,bot_username=?,webhook_secret=?,generation=?", sealedToken, config.username, sealedWebhook, config.generation, sealedToken, config.username, sealedWebhook, config.generation);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.configure',true)", actor.id);
    co_await tx.commit();
    co_return true;
}
ruvia::task<bool> TelegramService::disable(ruvia::db_executor db, UserIdentity actor) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!(co_await activeUser(tx, actor, true))) {
        co_return false;
    }
    co_await tx.execute("DELETE FROM telegram_settings");
    co_await tx.execute("DELETE FROM telegram_bindings");
    co_await tx.execute("DELETE FROM identity_telegram_verifications");
    co_await tx.execute("DELETE FROM telegram_accounts");
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE state IN ('pending','approved')");
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.disable',true)", actor.id);
    co_await tx.commit();
    co_return true;
}
ruvia::task<std::optional<std::string>> TelegramService::beginBinding(ruvia::db_executor db, UserIdentity user) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!(co_await activeUser(tx, user))) {
        co_return std::nullopt;
    }
    if (co_await tx.repository<TelegramAccountEntity>().exists({.where_ = TelegramAccountEntity::column<"user_id">() == user.id})) {
        co_return std::nullopt;
    }
    const auto bot = co_await tx.repository<TelegramSettingsEntity>().find_first({});
    if (!bot) {
        co_return std::nullopt;
    }
    const auto token = randomToken();
    co_await tx.execute("DELETE FROM telegram_bindings WHERE user_id=? OR expires_at<=UTC_TIMESTAMP(6)", user.id);
    co_await tx.execute("INSERT INTO telegram_bindings(token_hash,user_id,login_token_hash,expires_at) VALUES(?,?,?,UTC_TIMESTAMP(6)+INTERVAL 10 MINUTE)", tokenHash(token), user.id, user.loginHash);
    co_await tx.commit();
    co_return "https://t.me/" + bot->get<"bot_username">() + "?start=" + token;
}
ruvia::task<std::optional<std::string>> TelegramService::bind(ruvia::db_executor db, std::string generation, std::string token, std::int64_t telegramId) const {
    if (!validToken(token) || telegramId <= 0) {
        co_return std::nullopt;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!(co_await tx.repository<TelegramSettingsEntity>().exists({.where_ = TelegramSettingsEntity::column<"generation">() == generation}))) {
        co_return std::nullopt;
    }
    const auto rows = co_await tx.query("SELECT b.user_id FROM telegram_bindings b JOIN users u ON u.id=b.user_id JOIN login_sessions s ON s.token_hash=b.login_token_hash AND s.user_id=b.user_id WHERE b.token_hash=? AND b.expires_at>UTC_TIMESTAMP(6) AND s.expires_at>UTC_TIMESTAMP(6) AND NOT u.disabled FOR UPDATE", tokenHash(token));
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto user = dbField<std::string>(rows[0][0]);
    if (co_await tx.repository<TelegramAccountEntity>().exists({.where_ = TelegramAccountEntity::column<"user_id">() == user || TelegramAccountEntity::column<"telegram_id">() == telegramId})) {
        co_return std::nullopt;
    }
    TelegramAccountEntity account;
    account.set<"user_id">(user).set<"telegram_id">(telegramId);
    co_await tx.repository<TelegramAccountEntity>().insert(account);
    co_await tx.repository<TelegramBindingEntity>().delete_by(TelegramBindingEntity::column<"user_id">() == user);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.bind',true)", user);
    co_await tx.commit();
    co_return user;
}
ruvia::task<void> TelegramService::unbind(ruvia::db_executor db, UserIdentity user) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    (void)co_await tx.query("SELECT name FROM operation_locks WHERE name='telegram' FOR UPDATE");
    if (!(co_await activeUser(tx, user))) {
        co_return;
    }
    co_await tx.repository<IdentityTelegramVerificationEntity>().delete_by(IdentityTelegramVerificationEntity::column<"user_id">() == user.id);
    co_await tx.repository<TelegramAccountEntity>().delete_by(TelegramAccountEntity::column<"user_id">() == user.id);
    co_await tx.repository<TelegramBindingEntity>().delete_by(TelegramBindingEntity::column<"user_id">() == user.id);
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE user_id=? AND state IN ('pending','approved')", user.id);
    co_await tx.execute("INSERT INTO audit_events(user_id,event_type,success) VALUES(?,'telegram.unbind',true)", user.id);
    co_await tx.commit();
}
ruvia::task<std::optional<ApprovalDispatch>> TelegramService::requestApproval(ruvia::db_executor db, UserIdentity user, std::string deviceId, bool writeControl) const {
    if (!writeControl) {
        co_return std::nullopt;
    }
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto devRows = co_await tx.query("SELECT id FROM devices WHERE id=? AND revoked_at IS NULL FOR UPDATE", deviceId);
    if (devRows.empty() || !(co_await activeUser(tx, user))) {
        co_return std::nullopt;
    }
    const auto rows = co_await tx.query("SELECT d.name,g.revision,u.username,r.telegram_id,b.token,b.generation FROM devices d JOIN device_grants g ON g.device_id=d.id AND g.user_id=? JOIN users u ON u.id=g.user_id JOIN telegram_accounts r ON r.user_id=u.id CROSS JOIN telegram_settings b WHERE d.id=? AND g.desktop AND ? AND g.write_control AND g.verification_method='telegram'", user.id, deviceId, writeControl);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE user_id=? AND state IN ('pending','approved') AND expires_at<=UTC_TIMESTAMP(6)", user.id);
    co_await tx.execute("DELETE FROM connection_verifications WHERE created_at<UTC_TIMESTAMP(6)-INTERVAL 7 DAY");
    // Bound notification volume per requester, including retries after cancellation.
    const auto recent = co_await tx.query("SELECT count(*) FROM connection_verifications WHERE user_id=? AND created_at>UTC_TIMESTAMP(6)-INTERVAL 1 MINUTE", user.id);
    if (dbField<int>(recent[0][0]) >= 5) {
        co_return std::nullopt;
    }
    const auto existing = co_await tx.query("SELECT id,write_control,TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at) FROM connection_verifications WHERE user_id=? AND device_id=? AND login_token_hash=? AND state='pending'", user.id, deviceId, user.loginHash);
    if (!existing.empty()) {
        ApprovalDispatch result{{dbField<std::string>(existing[0][0]), deviceId, "pending", dbField<bool>(existing[0][1]), dbField<std::int64_t>(existing[0][2])}, {}, {}, {}, 0};
        co_await tx.commit();
        co_return result;
    }
    co_await tx.execute("UPDATE connection_verifications SET state='cancelled' WHERE user_id=? AND device_id=? AND state IN ('pending','approved')", user.id, deviceId);
    const auto id = randomToken().substr(0, 32);
    const auto inserted = co_await tx.query("INSERT INTO connection_verifications(id,user_id,device_id,login_token_hash,write_control,grant_revision,requester_telegram_id,bot_generation,state,expires_at) VALUES(?,?,?,?,?,?,?,?,'pending',UTC_TIMESTAMP(6)+INTERVAL 5 MINUTE) RETURNING TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at)", id, user.id, deviceId, user.loginHash, writeControl, dbField<std::int64_t>(rows[0][1]), dbField<std::int64_t>(rows[0][3]), dbField<std::string>(rows[0][5]));
    ApprovalDispatch result{{id, deviceId, "pending", writeControl, dbField<std::int64_t>(inserted[0][0])}, openSecret(key_, "telegram:token", dbField<std::string>(rows[0][4])), dbField<std::string>(rows[0][2]), dbField<std::string>(rows[0][0]), dbField<std::int64_t>(rows[0][3])};
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'connection.request',true,?)", user.id, deviceId, id);
    co_await tx.commit();
    co_return result;
}
ruvia::task<void> TelegramService::deliveryFailed(ruvia::db_executor db, std::string id) const {
    ConnectionVerificationEntity failed;
    failed.set<"state">("failed");
    co_await db.repository<ConnectionVerificationEntity>().update(ConnectionVerificationEntity::column<"id">() == id && ConnectionVerificationEntity::column<"state">() == "pending", failed);
}
ruvia::task<std::optional<std::string>> TelegramService::decide(ruvia::db_executor db, std::string generation, std::string id, std::int64_t telegramId, bool accept) const {
    auto tx = co_await db.begin_transaction(kReadCommitted);
    const auto sql = "UPDATE connection_verifications SET state=?,decided_at=UTC_TIMESTAMP(6) WHERE id=? AND requester_telegram_id=? AND bot_generation=? AND state='pending' AND id IN (" + std::string(kValidApproval) + ")";
    const auto changed = co_await tx.execute(sql, accept ? "approved" : "denied", id, telegramId, generation);
    if (changed.affected_rows() == 0) {
        co_return std::nullopt;
    }
    const auto rows = co_await tx.query("SELECT user_id,device_id FROM connection_verifications WHERE id=?", id);
    const auto userId = dbField<std::string>(rows[0][0]);
    co_await tx.execute("INSERT INTO audit_events(user_id,device_id,event_type,success,reason) VALUES(?,?,'connection.decide',?,?)", userId, dbField<std::string>(rows[0][1]), accept, id);
    co_await tx.commit();
    co_return userId;
}
ruvia::task<std::vector<ApprovalInfo>> TelegramService::approvals(ruvia::db_executor db, UserIdentity user) const {
    const auto sql = "SELECT id,device_id,CASE WHEN state IN ('pending','approved') AND (expires_at<=UTC_TIMESTAMP(6) OR id NOT IN (" + std::string(kValidApproval) + ")) THEN 'cancelled' ELSE state END,write_control,TIMESTAMPDIFF(SECOND,'1970-01-01 00:00:00',expires_at) FROM connection_verifications WHERE user_id=? AND login_token_hash=? ORDER BY created_at DESC LIMIT 20";
    const auto rows = co_await db.query(sql, user.id, user.loginHash);
    std::vector<ApprovalInfo> result;
    for (const auto& row : rows) {
        result.push_back({dbField<std::string>(row[0]), dbField<std::string>(row[1]), dbField<std::string>(row[2]), dbField<bool>(row[3]), dbField<std::int64_t>(row[4])});
    }
    co_return result;
}
ruvia::task<void> TelegramService::cancel(ruvia::db_executor db, UserIdentity user, std::string id) const {
    co_await db.execute("UPDATE connection_verifications SET state='cancelled' WHERE id=? AND user_id=? AND login_token_hash=? AND state IN ('pending','approved')", id, user.id, user.loginHash);
}
ruvia::task<bool> TelegramService::consumeApproval(ruvia::db_transaction& tx, const SessionRequest& request, std::int64_t revision) {
    if (request.kind != wire::DESKTOP || request.approvalId.size() != 32) {
        co_return false;
    }
    const auto sql = "UPDATE connection_verifications SET state='consumed' WHERE id=? AND user_id=? AND device_id=? AND login_token_hash=? AND write_control=? AND grant_revision=? AND state='approved' AND id IN (" + std::string(kValidApproval) + ")";
    const auto updated = co_await tx.execute(sql, request.approvalId, request.userId, request.deviceId, request.loginHash, request.writeControl, revision);
    co_return updated.affected_rows() != 0;
}
} // namespace ultraterimal::server
