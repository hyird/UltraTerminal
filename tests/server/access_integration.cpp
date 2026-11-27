// Manual integration check against a disposable, migrated MariaDB database.
// Deliberately not registered with CTest/CI. Connection settings arrive on stdin.
#include <chrono>
#include <db_executor.h>
#include <event_loop.h>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "common/database.h"
#include "config/storage.h"
#include "modules/devices/device.service.h"
#include "modules/identity/identity.service.h"
#include "modules/identity/security.protocol.h"
#include "modules/sessions/session.service.h"
#include "modules/telegram/telegram.service.h"

using namespace ultraterimal::server;
namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
ruvia::task<void> runIntegration(ruvia::db_executor db, const char*& phase) {
    phase = "MariaDB transactions, binary values and gateway lease";
    {
        const std::string text = "Unicode 中文 ' ? ; \\", binary("a\0b\xff", 4);
        const auto values = co_await db.query("SELECT ? AS text_value,? AS binary_value,? AS null_value,? AS number_value", text, ruvia::as_bytes(std::string_view(binary)), nullptr, std::int64_t{-123});
        const auto rawBytes = values[0]["binary_value"].bytes();
        const std::string decodedBinary(reinterpret_cast<const char*>(rawBytes->data()), rawBytes->size());
        check(dbField<std::string>(values[0]["text_value"]) == text && decodedBinary == binary && values[0]["null_value"].is_null() && dbField<int>(values[0]["number_value"]) == -123, "Prepared values preserve bytes and types");
        {
            auto tx = co_await db.begin_transaction(kReadCommitted);
            co_await tx.execute("INSERT INTO operation_locks(name) VALUES('rollback_test')");
            co_await tx.rollback();
        }
        const auto rolledBack = co_await db.query("SELECT name FROM operation_locks WHERE name='rollback_test'");
        check(rolledBack.empty(), "Transaction rollback");
        const auto leaseOwner = randomToken();
        const auto otherOwner = randomToken();
        check(co_await acquireGatewayLease(db, leaseOwner), "Acquire gateway lease");
        check(co_await renewGatewayLease(db, leaseOwner), "Gateway lease alive");
        check(!(co_await acquireGatewayLease(db, otherOwner)), "Duplicate gateway rejected");
        co_await releaseGatewayLease(db, leaseOwner);
    }
    const auto key = randomToken(), password = randomToken();
    IdentityService identities(key);
    DeviceService devices;
    SessionService sessions(key);
    TelegramService telegram(key);
    co_await identities.bootstrap(db, "owner", password);
    auto owner = (co_await identities.login(db, "owner", password)).value();
    check(owner.user.role == "admin", "Bootstrap role");
    check(co_await identities.createAccount(db, owner.user, "guest", password, "user"), "Create account");
    auto guest = (co_await identities.login(db, "guest", password)).value();
    check(!(co_await identities.accounts(db, guest.user)) && !(co_await identities.createAccount(db, guest.user, "evil", password, "admin")), "Admin isolation");
    check(!(co_await identities.updateAccount(db, owner.user, owner.user.id, "user", false, {})), "Last admin protection");
    phase = "administrator device ownership transfer";
    const auto transferredDevice = co_await devices.createDevice(db, owner.user.id, "transfer target");
    const auto obsoleteEnrollment = (co_await devices.createEnrollment(db, owner.user.id, transferredDevice.id)).value();
    check(!(co_await devices.transferOwner(db, guest.user.id, transferredDevice.id, guest.user.id)), "Non-admin ownership transfer rejected");
    check(!(co_await devices.transferOwner(db, owner.user.id, transferredDevice.id, "missing-user")), "Unknown owner rejected");
    check(co_await devices.transferOwner(db, owner.user.id, transferredDevice.id, guest.user.id), "Admin transfers ownership");
    check(!(co_await devices.transferOwner(db, owner.user.id, transferredDevice.id, guest.user.id)), "Unchanged owner rejected");
    check(!(co_await devices.enrollmentDevice(db, obsoleteEnrollment)) && !(co_await devices.createEnrollment(db, owner.user.id, transferredDevice.id)), "Former owner enrollment revoked");
    check((co_await devices.find(db, guest.user.id, transferredDevice.id))->canManage && (co_await devices.rename(db, guest.user.id, transferredDevice.id, "new owner device")), "New owner can manage device");
    check(!(co_await devices.find(db, owner.user.id, transferredDevice.id)) && !(co_await devices.rename(db, owner.user.id, transferredDevice.id, "former owner")), "Former owner loses device access");
    check(co_await devices.transferOwner(db, owner.user.id, transferredDevice.id, owner.user.id), "Admin can transfer device back");
    check(!(co_await devices.find(db, guest.user.id, transferredDevice.id)), "Former target grant revoked");
    phase = "device grants and session binding";
    const auto device = co_await devices.createDevice(db, owner.user.id, "test device");
    check((co_await devices.list(db, guest.user.id)).empty() && !(co_await devices.rename(db, guest.user.id, device.id, "stolen")) && !(co_await devices.createEnrollment(db, guest.user.id, device.id)), "Ownership isolation");
    const auto enrollment = (co_await devices.createEnrollment(db, owner.user.id, device.id)).value();
    check((co_await devices.enrollmentDevice(db, enrollment)) == device.id, "Enrollment target");
    const auto registration = (co_await devices.registerDevice(db, enrollment, "test device")).value();
    const auto agent = (co_await devices.authenticate(db, registration.credential)).value();
    check(!(co_await devices.registerDevice(db, enrollment, "replay")), "Enrollment single use");
    check((co_await devices.heartbeat(db, agent)) && (co_await devices.capabilities(db, agent, std::string("a\0b", 3))), "Agent heartbeat and binary capabilities");
    check(co_await devices.rename(db, owner.user.id, device.id, "中文设备"), "Owner rename");
    check((co_await devices.list(db, owner.user.id)).front().lastSeen.value().ends_with("Z"), "Device timestamps use UTC");
    const auto restartKey = randomToken();
    const auto restart = (co_await devices.createRestart(db, owner.user.id, device.id, restartKey, 123)).value();
    check((co_await devices.createRestart(db, owner.user.id, device.id, restartKey, 123))->id == restart.id, "Restart idempotency");
    ultraterimal::wire::WindowsRestartResult restartResult;
    restartResult.set_operation_id(restart.id);
    restartResult.set_status(ultraterimal::wire::WindowsRestartResult::ACCEPTED);
    check((co_await devices.applyRestartResult(db, agent, restartResult)) && !(co_await devices.applyRestartResult(db, agent, restartResult)), "Restart result single application");
    check((co_await devices.restart(db, owner.user.id, restart.id))->status == "ACCEPTED", "Restart result stored");
    const auto failedRestart = (co_await devices.createRestart(db, owner.user.id, device.id, randomToken(), 123)).value();
    co_await devices.updateRestart(db, failedRestart.id, "FAILED", 5);
    check((co_await devices.restart(db, owner.user.id, failedRestart.id))->status == "FAILED", "Restart dispatch failure");
    const auto pendingRestart = (co_await devices.createRestart(db, owner.user.id, device.id, randomToken(), 123)).value();
    co_await devices.recoverRestarts(db);
    check((co_await devices.restart(db, owner.user.id, pendingRestart.id))->status == "UNKNOWN", "Restart recovery");
    DeviceGrant grant{{}, "guest", true, false, false, false, "direct"};
    check(!(co_await devices.setGrant(db, owner.user.id, device.id, grant, false)), "Read-only grants are rejected");
    grant.writeControl = true;
    check(co_await devices.setGrant(db, owner.user.id, device.id, grant, false), "Share controlled device");
    check(!(co_await devices.canConfigureWindowsLogin(db, guest.user.id, device.id)) && !(co_await devices.systemRestartPermission(db, guest.user.id, device.id)).value_or(true), "Administrative device permissions");
    SessionRequest request;
    request.userId = guest.user.id;
    request.deviceId = device.id;
    request.kind = ultraterimal::wire::DESKTOP;
    request.loginHash = guest.user.loginHash;
    request.preempt = true;
    request.writeControl = false;
    request.desktopInputContract = 0;
    request.agentInputContractSupported = true;
    check(!(co_await sessions.create(db, request)), "Read-only session requests are rejected");
    check(!(co_await telegram.requestApproval(db, guest.user, device.id, false)), "Read-only approvals are rejected");
    request.writeControl = true;
    request.desktopInputContract = 2;
    auto issued = (co_await sessions.create(db, request)).value();
    const auto secondLogin = (co_await identities.login(db, "guest", password)).value();
    const auto& channel = issued.channels.front();
    check(!(co_await sessions.consumeBrowser(db, secondLogin.user, issued.id, channel.id, channel.purpose, channel.browserTicket)), "Ticket bound to login");
    check((co_await sessions.consumeBrowser(db, guest.user, issued.id, channel.id, channel.purpose, channel.browserTicket)).has_value(), "Valid channel ticket");
    check(!(co_await sessions.consumeBrowser(db, guest.user, issued.id, channel.id, channel.purpose, channel.browserTicket)), "Ticket single use");
    for (const auto& item : issued.channels) {
        if (item.id != channel.id) {
            check((co_await sessions.consumeBrowser(db, guest.user, issued.id, item.id, item.purpose, item.browserTicket)).has_value(), "Remaining browser channels");
        }
        check((co_await sessions.consumeAgent(db, agent, issued.id, item.id, item.purpose, item.agentTicket)).has_value(), "Agent channel ticket");
    }
    check((co_await sessions.get(db, guest.user.id, issued.id))->state == "active", "Session activated after both peers");
    co_await identities.logout(db, guest.token);
    check(!(co_await sessions.authorized(db, issued.id)) && !(co_await sessions.revalidate(db, guest.user.id)).empty(), "Logout terminates session");
    guest = (co_await identities.login(db, "guest", password)).value();
    request.loginHash = guest.user.loginHash;
    issued = (co_await sessions.create(db, request)).value();
    check((co_await devices.setGrant(db, owner.user.id, device.id, grant, false)) && !(co_await sessions.authorized(db, issued.id)), "Grant revision invalidates session");
    (void)co_await sessions.revalidate(db);
    check((co_await devices.setGrant(db, owner.user.id, device.id, grant, true)) && !(co_await sessions.create(db, request)), "Grant revocation");
    check(co_await devices.setGrant(db, owner.user.id, device.id, grant, false), "Grant restoration");
    phase = "Telegram bindings and approvals";
    const TelegramConfig bot{"123456789:abcdefghijklmnopqrstuvwxyz123456789", "validation_bot", randomToken(), randomToken()};
    check(co_await telegram.configure(db, owner.user, bot), "Configure bot storage");
    const auto ownerLink = (co_await telegram.beginBinding(db, owner.user)).value();
    const auto guestLink = (co_await telegram.beginBinding(db, guest.user)).value();
    const auto ownerToken = ownerLink.substr(ownerLink.find("?start=") + 7), guestToken = guestLink.substr(guestLink.find("?start=") + 7);
    check((co_await telegram.bind(db, bot.generation, ownerToken, 10001)).has_value(), "Owner binding");
    check(!(co_await telegram.bind(db, bot.generation, ownerToken, 10002)) && !(co_await telegram.bind(db, bot.generation, guestToken, 10001)), "Binding replay and unique Telegram account");
    auto unboundTelegramGrant = grant;
    unboundTelegramGrant.verificationMethod = "telegram";
    check(!(co_await devices.setGrant(db, owner.user.id, device.id, unboundTelegramGrant, false)), "Owner binding cannot replace missing requester binding");
    check((co_await telegram.bind(db, bot.generation, guestToken, 10002)).has_value(), "Requester binding");
    co_await telegram.unbind(db, owner.user);
    grant.verificationMethod = "telegram";
    check(co_await devices.setGrant(db, owner.user.id, device.id, grant, false), "Requester verification does not require owner Telegram binding");
    check((co_await sessions.create(db, request)).error() == SessionFailure::kApprovalRequired, "Approval cannot be bypassed");
    auto approval = (co_await telegram.requestApproval(db, guest.user, device.id, true)).value();
    check(approval.requesterTelegramId == 10002, "Connection confirmation goes to requester Telegram account");
    check(!(co_await telegram.decide(db, bot.generation, approval.approval.id, 10001, true)), "Device owner cannot confirm requester identity");
    check((co_await telegram.decide(db, bot.generation, approval.approval.id, 10002, true)).has_value(), "Requester confirms own connection");
    check(!(co_await telegram.decide(db, bot.generation, approval.approval.id, 10002, true)), "Callback replay");
    request.approvalId = approval.approval.id;
    issued = (co_await sessions.create(db, request)).value();
    check(!(co_await sessions.create(db, request)), "Approval consumed once");
    (void)co_await sessions.close(db, guest.user.id, issued.id, SessionEnd::kUser);
    approval = (co_await telegram.requestApproval(db, guest.user, device.id, true)).value();
    check((co_await telegram.decide(db, bot.generation, approval.approval.id, 10002, true)).has_value(), "Second approval");
    co_await telegram.unbind(db, guest.user);
    request.approvalId = approval.approval.id;
    check(!(co_await sessions.create(db, request)), "Unbinding invalidates approval");
    const auto reboundOwner = (co_await telegram.beginBinding(db, owner.user)).value();
    check((co_await telegram.bind(db, bot.generation, reboundOwner.substr(reboundOwner.find("?start=") + 7), 10001)).has_value(), "Restore owner personal Telegram binding");
    phase = "Telegram identity verification";
    const auto tgProof = (co_await identities.requestTelegramVerification(db, owner.user, bot.generation)).value();
    check(!(co_await identities.reauthenticate(db, owner.user, {}, {}, tgProof.id)), "Pending Telegram proof rejected");
    check(!(co_await identities.decideTelegramVerification(db, bot.generation, tgProof.id, 10002, true)), "Foreign Telegram identity rejected");
    check((co_await identities.decideTelegramVerification(db, bot.generation, tgProof.id, 10001, true)).has_value(), "Telegram identity approved");
    check(!(co_await identities.reauthenticate(db, guest.user, {}, {}, tgProof.id)), "Telegram proof belongs to requesting account");
    const auto anotherOwnerLogin = (co_await identities.login(db, "owner", password)).value();
    check(!(co_await identities.reauthenticate(db, anotherOwnerLogin.user, {}, {}, tgProof.id)), "Telegram proof belongs to requesting login session");
    check(!(co_await identities.requestTelegramVerification(db, owner.user, bot.generation)), "Telegram confirmation request cooldown");
    check(co_await identities.reauthenticate(db, owner.user, {}, {}, tgProof.id), "Telegram-only operation verification");
    check(!(co_await identities.reauthenticate(db, owner.user, {}, {}, tgProof.id)), "Telegram proof consumed once");
    check(co_await identities.reauthenticate(db, owner.user, password, {}), "Password remains alternative proof");
    phase = "TOTP and recovery codes";
    const auto setup = (co_await identities.changeSecurity(db, guest.user, "begin", password, {})).value();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto code = totpCode(setup.secret, now / 30);
    const auto activated = (co_await identities.changeSecurity(db, guest.user, "confirm", {}, code)).value();
    check(activated.recoveryCodes.size() == 10 && (co_await identities.security(db, guest.user.id)).totp, "Enable TOTP");
    const auto challenge = co_await identities.login(db, "guest", password);
    check(!challenge && challenge.error() == LoginError::kFactorRequired, "Verified password requires second factor");
    const auto invalid = co_await identities.login(db, "guest", "incorrect-password");
    check(!invalid && invalid.error() == LoginError::kInvalidCredentials, "Wrong password never discloses second factor");
    check(!(co_await identities.login(db, "guest", password)) && !(co_await identities.login(db, "guest", password, code)), "TOTP required and replay blocked");
    check(co_await identities.reauthenticate(db, guest.user, password, {}), "Password-only operation proof with TOTP enabled");
    check(!(co_await identities.reauthenticate(db, guest.user, {}, {})), "Empty operation proof rejected");
    check(!(co_await identities.reauthenticate(db, guest.user, password, code)), "Ambiguous operation proof rejected");
    check(!(co_await identities.reauthenticate(db, guest.user, {}, activated.recoveryCodes.front())), "Recovery code not an operation authenticator");
    check(co_await identities.reauthenticate(db, guest.user, {}, totpCode(setup.secret, now / 30 + 1)), "TOTP-only operation proof");
    check(!(co_await identities.reauthenticate(db, guest.user, {}, totpCode(setup.secret, now / 30 + 1))), "Operation TOTP replay rejected");
    check(co_await identities.reauthenticate(db, guest.user, password, {}), "Password proof resets failed attempts");
    check((co_await identities.login(db, "guest", password, activated.recoveryCodes.front())).has_value(), "Recovery login");
    check(!(co_await identities.login(db, "guest", password, activated.recoveryCodes.front())), "Recovery single use");
    phase = "per-user connection verification";
    check(co_await identities.createAccount(db, owner.user, "otpuser", password, "user"), "Create connection OTP user");
    const auto otpUser = (co_await identities.login(db, "otpuser", password)).value();
    const auto otpSetup = (co_await identities.changeSecurity(db, otpUser.user, "begin", password, {})).value();
    check((co_await identities.changeSecurity(db, otpUser.user, "confirm", {}, totpCode(otpSetup.secret, now / 30 - 1))).has_value(), "Bind connection OTP user");
    DeviceGrant otpGrant{{}, "otpuser", true, true, false, false, "totp"};
    check(co_await devices.setGrant(db, owner.user.id, device.id, otpGrant, false), "Per-user TOTP policy");
    auto otpRequest = request;
    otpRequest.userId = otpUser.user.id;
    otpRequest.loginHash = otpUser.user.loginHash;
    otpRequest.approvalId.clear();
    check((co_await sessions.create(db, otpRequest)).error() == SessionFailure::kVerificationRequired, "TOTP connection requires proof");
    otpRequest.verificationCode = totpCode(otpSetup.secret, now / 30);
    const auto otpSession = (co_await sessions.create(db, otpRequest)).value();
    (void)co_await sessions.close(db, otpUser.user.id, otpSession.id, SessionEnd::kUser);
    check((co_await sessions.create(db, otpRequest)).error() == SessionFailure::kVerificationFailed, "Connection OTP cannot be replayed");
    phase = "five minute reconnect authorization";
    auto reconnectRequest = otpRequest;
    reconnectRequest.preempt = false;
    reconnectRequest.verificationCode.clear();
    reconnectRequest.reconnectSessionId = otpSession.id;
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "User close cannot skip verification");
    co_await db.execute("UPDATE remote_sessions SET end_reason='session_failed',ended_at=UTC_TIMESTAMP(6)-INTERVAL 301 SECOND WHERE id=?", otpSession.id);
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Reconnect after five minutes requires verification");
    co_await db.execute("UPDATE remote_sessions SET ended_at=UTC_TIMESTAMP(6)-INTERVAL 299 SECOND WHERE id=?", otpSession.id);
    const auto otherOtpLogin = (co_await identities.login(db, "otpuser", password, totpCode(otpSetup.secret, now / 30 + 1))).value();
    reconnectRequest.loginHash = otherOtpLogin.user.loginHash;
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Reconnect is bound to original login");
    reconnectRequest.loginHash = otpUser.user.loginHash;
    co_await db.execute("UPDATE remote_sessions SET grant_revision=grant_revision-1 WHERE id=?", otpSession.id);
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Changed grant cannot skip verification");
    co_await db.execute("UPDATE remote_sessions SET grant_revision=grant_revision+1,ended_at=UTC_TIMESTAMP(6) WHERE id=?", otpSession.id);
    const auto resumedOtp = (co_await sessions.create(db, reconnectRequest)).value();
    const auto resumedRows = co_await db.query("SELECT reconnect_until IS NOT NULL FROM remote_sessions WHERE id=?", resumedOtp.id);
    check(dbField<bool>(resumedRows[0][0]), "Pending retry inherits fixed reconnect deadline");
    (void)co_await sessions.close(db, otpUser.user.id, resumedOtp.id, SessionEnd::kFailure);
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Reconnect source is single use");
    reconnectRequest.reconnectSessionId = resumedOtp.id;
    co_await db.execute("UPDATE remote_sessions SET reconnect_until=UTC_TIMESTAMP(6)-INTERVAL 1 SECOND WHERE id=?", resumedOtp.id);
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Failed retry cannot extend original deadline");
    co_await db.execute("UPDATE remote_sessions SET reconnect_until=UTC_TIMESTAMP(6)+INTERVAL 5 MINUTE,end_reason='preempted' WHERE id=?", resumedOtp.id);
    check((co_await sessions.create(db, reconnectRequest)).error() == SessionFailure::kVerificationRequired, "Preempted session cannot skip verification");
    DeviceGrant ownPolicy{{}, "owner", true, true, false, false, "telegram"};
    check(co_await devices.setGrant(db, owner.user.id, device.id, ownPolicy, false), "Owner can configure own policy without granular permission flags");
    auto ownerRequest = request;
    ownerRequest.userId = owner.user.id;
    ownerRequest.loginHash = owner.user.loginHash;
    ownerRequest.approvalId.clear();
    check((co_await sessions.create(db, ownerRequest)).error() == SessionFailure::kApprovalRequired, "Owner obeys own grant policy");
    ownPolicy.verificationMethod = "direct";
    check(co_await devices.setGrant(db, owner.user.id, device.id, ownPolicy, false), "Restore owner direct policy");
    phase = "disabled accounts and failure limits";
    check(co_await identities.updateAccount(db, owner.user, guest.user.id, "user", true, {}), "Disable user");
    check(!(co_await identities.authenticate(db, guest.token)) && !(co_await identities.login(db, "guest", password, activated.recoveryCodes[1])), "Disabled user rejected");
    check(co_await identities.createAccount(db, owner.user, "limited", password, "user"), "Rate test account");
    for (int i = 0; i < 5; ++i) {
        check(!(co_await identities.login(db, "limited", "incorrect-password")), "Invalid login");
    }
    check(!(co_await identities.login(db, "limited", password)), "Account cooldown");
    phase = "administrator machine allocation";
    check(co_await identities.createAccount(db, owner.user, "operator", password, "admin"), "Create allocation administrator");
    check(co_await identities.createAccount(db, owner.user, "recipient", password, "user"), "Create allocation recipient");
    const auto operatorLogin = (co_await identities.login(db, "operator", password)).value();
    const auto recipient = (co_await identities.login(db, "recipient", password)).value();
    const auto privateDevice = co_await devices.createDevice(db, recipient.user.id, "recipient machine");
    check((co_await devices.list(db, operatorLogin.user.id)).empty(), "Administrator receives no implicit desktop access");
    check((co_await devices.find(db, operatorLogin.user.id, privateDevice.id, true)).has_value(), "Administrator can discover another owner's machine");
    check((co_await devices.list(db, recipient.user.id, true)).empty() && !(co_await devices.find(db, recipient.user.id, device.id, true)), "Ordinary user cannot request administrative scope");
    check(!(co_await devices.grants(db, recipient.user.id, device.id)), "Ordinary user cannot read another owner's grants");
    DeviceGrant allocation{{}, "recipient", true, true, false, false, "direct"};
    check(co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, false), "Administrator assigns another owner's machine");
    check((co_await devices.find(db, recipient.user.id, device.id))->writeControl, "Assigned user receives control");
    check(!(co_await devices.setGrant(db, recipient.user.id, device.id, allocation, false)), "Recipient cannot delegate grants");
    DeviceGrant ownerGrant{{}, "owner", true, true, true, true, "direct"};
    check(!(co_await devices.setGrant(db, operatorLogin.user.id, device.id, ownerGrant, true)), "Administrator cannot remove the owner's grant");
    check(!(co_await devices.canConfigureWindowsLogin(db, recipient.user.id, device.id)), "Connection grant cannot configure credentials");
    check(!(co_await devices.canConfigureWindowsLogin(db, operatorLogin.user.id, device.id)), "Non-owner administrator cannot configure credentials");
    check(co_await devices.canConfigureWindowsLogin(db, owner.user.id, device.id), "Owner can configure credentials");
    allocation.manageCredentials = true;
    check(!(co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, false)), "Credential permission cannot be delegated");
    allocation.manageCredentials = false;
    allocation.verificationMethod = "telegram";
    check(!(co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, false)), "Device owner binding does not verify an unbound recipient");
    const auto recipientLink = (co_await telegram.beginBinding(db, recipient.user)).value();
    check((co_await telegram.bind(db, bot.generation, recipientLink.substr(recipientLink.find("?start=") + 7), 10003)).has_value(), "Bind recipient Telegram identity");
    check(co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, false), "Telegram verification uses the allocated user's binding");
    check((co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, true)) && !(co_await devices.find(db, recipient.user.id, device.id)), "Administrator revokes allocation");
    check(co_await identities.updateAccount(db, owner.user, operatorLogin.user.id, "user", false, {}), "Demote administrator");
    check(!(co_await devices.setGrant(db, operatorLogin.user.id, device.id, allocation, false)) && !(co_await devices.grants(db, operatorLogin.user.id, device.id)), "Database role revocation is authoritative");
}
} // namespace
int main() {
    const char* phase = "initialization";
    try {
        std::string settings;
        std::getline(std::cin, settings);
        const auto database = parseDatabaseConfig(settings);
        check(!settings.empty(), "Missing test database");
        migrateDatabase(database);
        migrateDatabase(database);
        ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 16, .blocking_pool_ = ruvia::blocking_pool_options{.thread_count_ = 1}});
        pool.start();
        const auto loop = pool.loop(0);
        auto client = std::make_shared<ruvia::db_client>(loop, toRuviaDbConfig(database, 2));
        auto submitted = loop.submit([](std::shared_ptr<ruvia::db_client> client, const char*& phaseRef) -> ruvia::task<void> {
            co_await client->connect();
            std::exception_ptr failure;
            try {
                co_await runIntegration(client->db(), phaseRef);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client->shutdown();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }(client, phase));
        check(submitted.accepted(), "Cannot submit integration task");
        try {
            std::move(submitted).take_value().take_value();
        } catch (...) {
            pool.join();
            throw;
        }
        pool.join();
        std::cout << "Access integration passed: ownership, roles, grants, login-bound tickets, revocation, requester Telegram verification, TOTP and reconnect\n";
        return 0;
    } catch (const ruvia::db_error& error) {
        std::cerr << "Access integration database error " << error.code().message() << " in phase: " << phase << '\n';
        return 1;
    } catch (const std::exception&) {
        std::cerr << "Access integration failed in phase: " << phase << '\n';
        return 1;
    }
}
