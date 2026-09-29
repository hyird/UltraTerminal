// Manual integration check against a disposable, migrated MariaDB database.
// Deliberately not registered with CTest/CI. Connection settings arrive on stdin.
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "modules/database/database.service.h"
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
        phase = "MariaDB transactions, binary values and gateway lease";
        {
            auto connection = openDatabase(database);
            const std::string text = "Unicode 中文 ' ? ; \\", binary("a\0b\xff", 4);
            const auto values = connection.exec("SELECT ? AS text_value,? AS binary_value,? AS null_value,? AS number_value", text, std::span<const std::byte>(reinterpret_cast<const std::byte*>(binary.data()), binary.size()), std::nullopt, std::int64_t{-123});
            check(values[0]["text_value"].as<std::string>() == text && values[0]["binary_value"].as<std::string>() == binary && !values[0]["null_value"].as<std::optional<std::string>>() && values[0]["number_value"].as<int>() == -123, "Prepared values preserve bytes and types");
            {
                DatabaseTransaction tx(connection);
                tx.exec("INSERT INTO operation_locks(name) VALUES('rollback_test')");
            }
            check(connection.exec("SELECT name FROM operation_locks WHERE name='rollback_test'").empty(), "RAII transaction rollback");
            auto lease = acquireGatewayLease(database);
            check(gatewayLeaseAlive(lease), "Gateway lease alive");
            bool rejected = false;
            try {
                auto duplicate = acquireGatewayLease(database);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected, "Duplicate gateway rejected");
        }
        const auto key = randomToken(), password = randomToken();
        IdentityService identities(database, key);
        DeviceService devices(database);
        SessionService sessions(database, key);
        TelegramService telegram(database, key);
        identities.bootstrap("owner", password);
        auto owner = identities.login("owner", password).value();
        check(owner.user.role == "admin", "Bootstrap role");
        check(identities.createAccount(owner.user, "guest", password, "user"), "Create account");
        auto guest = identities.login("guest", password).value();
        check(!identities.accounts(guest.user) && !identities.createAccount(guest.user, "evil", password, "admin"), "Admin isolation");
        check(!identities.updateAccount(owner.user, owner.user.id, "user", false, {}), "Last admin protection");
        phase = "device grants and session binding";
        const auto device = devices.createDevice(owner.user.id, "test device");
        check(devices.list(guest.user.id).empty() && !devices.rename(guest.user.id, device.id, "stolen") && !devices.createEnrollment(guest.user.id, device.id), "Ownership isolation");
        const auto enrollment = devices.createEnrollment(owner.user.id, device.id).value();
        check(devices.enrollmentDevice(enrollment) == device.id, "Enrollment target");
        const auto registration = devices.registerDevice(enrollment, "test device").value();
        const auto agent = devices.authenticate(registration.credential).value();
        check(!devices.registerDevice(enrollment, "replay"), "Enrollment single use");
        check(devices.heartbeat(agent) && devices.capabilities(agent, std::string("a\0b", 3)), "Agent heartbeat and binary capabilities");
        check(devices.rename(owner.user.id, device.id, "中文设备"), "Owner rename");
        check(devices.list(owner.user.id).front().lastSeen.value().ends_with("Z"), "Device timestamps use UTC");
        const auto restartKey = randomToken();
        const auto restart = devices.createRestart(owner.user.id, device.id, restartKey, 123).value();
        check(devices.createRestart(owner.user.id, device.id, restartKey, 123)->id == restart.id, "Restart idempotency");
        ultraterimal::wire::WindowsRestartResult restartResult;
        restartResult.set_operation_id(restart.id);
        restartResult.set_status(ultraterimal::wire::WindowsRestartResult::ACCEPTED);
        check(devices.applyRestartResult(agent, restartResult) && !devices.applyRestartResult(agent, restartResult), "Restart result single application");
        check(devices.restart(owner.user.id, restart.id)->status == "ACCEPTED", "Restart result stored");
        const auto failedRestart = devices.createRestart(owner.user.id, device.id, randomToken(), 123).value();
        devices.updateRestart(failedRestart.id, "FAILED", 5);
        check(devices.restart(owner.user.id, failedRestart.id)->status == "FAILED", "Restart dispatch failure");
        const auto pendingRestart = devices.createRestart(owner.user.id, device.id, randomToken(), 123).value();
        devices.recoverRestarts();
        check(devices.restart(owner.user.id, pendingRestart.id)->status == "UNKNOWN", "Restart recovery");
        DeviceGrant grant{{}, "guest", true, false, false, false, "direct"};
        check(!devices.setGrant(owner.user.id, device.id, grant, false), "Read-only grants are rejected");
        grant.writeControl = true;
        check(devices.setGrant(owner.user.id, device.id, grant, false), "Share controlled device");
        check(!devices.canConfigureWindowsLogin(guest.user.id, device.id) && !devices.systemRestartPermission(guest.user.id, device.id).value_or(true), "Administrative device permissions");
        SessionRequest request;
        request.userId = guest.user.id;
        request.deviceId = device.id;
        request.kind = ultraterimal::wire::DESKTOP;
        request.loginHash = guest.user.loginHash;
        request.preempt = true;
        request.writeControl = true;
        request.desktopInputContract = 2;
        request.agentInputContractSupported = true;
        request.writeControl = false;
        request.desktopInputContract = 0;
        check(!sessions.create(request), "Read-only session requests are rejected");
        check(!telegram.requestApproval(guest.user, device.id, false), "Read-only approvals are rejected");
        request.writeControl = true;
        request.desktopInputContract = 2;
        auto issued = sessions.create(request).value();
        const auto secondLogin = identities.login("guest", password).value();
        const auto& channel = issued.channels.front();
        check(!sessions.consumeBrowser(secondLogin.user, issued.id, channel.id, channel.purpose, channel.browserTicket), "Ticket bound to login");
        check(sessions.consumeBrowser(guest.user, issued.id, channel.id, channel.purpose, channel.browserTicket).has_value(), "Valid channel ticket");
        check(!sessions.consumeBrowser(guest.user, issued.id, channel.id, channel.purpose, channel.browserTicket), "Ticket single use");
        for (const auto& item : issued.channels) {
            if (item.id != channel.id) {
                check(sessions.consumeBrowser(guest.user, issued.id, item.id, item.purpose, item.browserTicket).has_value(), "Remaining browser channels");
            }
            check(sessions.consumeAgent(agent, issued.id, item.id, item.purpose, item.agentTicket).has_value(), "Agent channel ticket");
        }
        check(sessions.get(guest.user.id, issued.id)->state == "active", "Session activated after both peers");
        identities.logout(guest.token);
        check(!sessions.authorized(issued.id) && !sessions.revalidate(guest.user.id).empty(), "Logout terminates session");
        guest = identities.login("guest", password).value();
        request.loginHash = guest.user.loginHash;
        issued = sessions.create(request).value();
        check(devices.setGrant(owner.user.id, device.id, grant, false) && !sessions.authorized(issued.id), "Grant revision invalidates session");
        sessions.revalidate();
        check(devices.setGrant(owner.user.id, device.id, grant, true) && !sessions.create(request), "Grant revocation");
        check(devices.setGrant(owner.user.id, device.id, grant, false), "Grant restoration");
        phase = "Telegram bindings and approvals";
        const TelegramConfig bot{"123456789:abcdefghijklmnopqrstuvwxyz123456789", "validation_bot", randomToken(), randomToken()};
        check(telegram.configure(owner.user, bot), "Configure bot storage");
        const auto ownerLink = telegram.beginBinding(owner.user).value(), guestLink = telegram.beginBinding(guest.user).value();
        const auto ownerToken = ownerLink.substr(ownerLink.find("?start=") + 7), guestToken = guestLink.substr(guestLink.find("?start=") + 7);
        check(telegram.bind(bot.generation, ownerToken, 10001).has_value(), "Owner binding");
        check(!telegram.bind(bot.generation, ownerToken, 10002) && !telegram.bind(bot.generation, guestToken, 10001), "Binding replay and unique Telegram account");
        check(telegram.bind(bot.generation, guestToken, 10002).has_value(), "Requester binding");
        grant.verificationMethod = "telegram";
        check(devices.setGrant(owner.user.id, device.id, grant, false), "Require approval");
        check(sessions.create(request).error() == SessionFailure::kApprovalRequired, "Approval cannot be bypassed");
        auto approval = telegram.requestApproval(guest.user, device.id, true).value();
        check(!telegram.decide(bot.generation, approval.approval.id, 10002, true), "Only owner decides");
        check(telegram.decide(bot.generation, approval.approval.id, 10001, true).has_value(), "Owner accepts");
        check(!telegram.decide(bot.generation, approval.approval.id, 10001, true), "Callback replay");
        request.approvalId = approval.approval.id;
        issued = sessions.create(request).value();
        check(!sessions.create(request), "Approval consumed once");
        sessions.close(guest.user.id, issued.id, SessionEnd::kUser);
        approval = telegram.requestApproval(guest.user, device.id, true).value();
        check(telegram.decide(bot.generation, approval.approval.id, 10001, true).has_value(), "Second approval");
        telegram.unbind(guest.user);
        request.approvalId = approval.approval.id;
        check(!sessions.create(request), "Unbinding invalidates approval");
        phase = "Telegram identity verification";
        const auto tgProof = identities.requestTelegramVerification(owner.user, bot.generation).value();
        check(!identities.reauthenticate(owner.user, {}, {}, tgProof.id), "Pending Telegram proof rejected");
        check(!identities.decideTelegramVerification(bot.generation, tgProof.id, 10002, true), "Foreign Telegram identity rejected");
        check(identities.decideTelegramVerification(bot.generation, tgProof.id, 10001, true).has_value(), "Telegram identity approved");
        check(!identities.reauthenticate(guest.user, {}, {}, tgProof.id), "Telegram proof belongs to requesting account");
        const auto anotherOwnerLogin = identities.login("owner", password).value();
        check(!identities.reauthenticate(anotherOwnerLogin.user, {}, {}, tgProof.id), "Telegram proof belongs to requesting login session");
        check(!identities.requestTelegramVerification(owner.user, bot.generation), "Telegram confirmation request cooldown");
        check(identities.reauthenticate(owner.user, {}, {}, tgProof.id), "Telegram-only operation verification");
        check(!identities.reauthenticate(owner.user, {}, {}, tgProof.id), "Telegram proof consumed once");
        check(identities.reauthenticate(owner.user, password, {}), "Password remains alternative proof");
        phase = "TOTP and recovery codes";
        const auto setup = identities.changeSecurity(guest.user, "begin", password, {}).value();
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto code = totpCode(setup.secret, now / 30);
        const auto activated = identities.changeSecurity(guest.user, "confirm", {}, code).value();
        check(activated.recoveryCodes.size() == 10 && identities.security(guest.user.id).totp, "Enable TOTP");
        const auto challenge = identities.login("guest", password);
        check(!challenge && challenge.error() == LoginError::kFactorRequired, "Verified password requires second factor");
        const auto invalid = identities.login("guest", "incorrect-password");
        check(!invalid && invalid.error() == LoginError::kInvalidCredentials, "Wrong password never discloses second factor");
        check(!identities.login("guest", password) && !identities.login("guest", password, code), "TOTP required and replay blocked");
        check(identities.reauthenticate(guest.user, password, {}), "Password-only operation proof with TOTP enabled");
        check(!identities.reauthenticate(guest.user, {}, {}), "Empty operation proof rejected");
        check(!identities.reauthenticate(guest.user, password, code), "Ambiguous operation proof rejected");
        check(!identities.reauthenticate(guest.user, {}, activated.recoveryCodes.front()), "Recovery code not an operation authenticator");
        check(identities.reauthenticate(guest.user, {}, totpCode(setup.secret, now / 30 + 1)), "TOTP-only operation proof");
        check(!identities.reauthenticate(guest.user, {}, totpCode(setup.secret, now / 30 + 1)), "Operation TOTP replay rejected");
        check(identities.reauthenticate(guest.user, password, {}), "Password proof resets failed attempts");
        check(identities.login("guest", password, activated.recoveryCodes.front()).has_value(), "Recovery login");
        check(!identities.login("guest", password, activated.recoveryCodes.front()), "Recovery single use");
        phase = "per-user connection verification";
        check(identities.createAccount(owner.user, "otpuser", password, "user"), "Create connection OTP user");
        const auto otpUser = identities.login("otpuser", password).value();
        const auto otpSetup = identities.changeSecurity(otpUser.user, "begin", password, {}).value();
        check(identities.changeSecurity(otpUser.user, "confirm", {}, totpCode(otpSetup.secret, now / 30 - 1)).has_value(), "Bind connection OTP user");
        DeviceGrant otpGrant{{}, "otpuser", true, true, false, false, "totp"};
        check(devices.setGrant(owner.user.id, device.id, otpGrant, false), "Per-user TOTP policy");
        auto otpRequest = request;
        otpRequest.userId = otpUser.user.id;
        otpRequest.loginHash = otpUser.user.loginHash;
        otpRequest.approvalId.clear();
        check(sessions.create(otpRequest).error() == SessionFailure::kVerificationRequired, "TOTP connection requires proof");
        otpRequest.verificationCode = totpCode(otpSetup.secret, now / 30);
        const auto otpSession = sessions.create(otpRequest).value();
        sessions.close(otpUser.user.id, otpSession.id, SessionEnd::kUser);
        check(sessions.create(otpRequest).error() == SessionFailure::kVerificationFailed, "Connection OTP cannot be replayed");
        DeviceGrant ownPolicy{{}, "owner", true, true, false, false, "telegram"};
        check(devices.setGrant(owner.user.id, device.id, ownPolicy, false), "Owner can configure own policy without granular permission flags");
        auto ownerRequest = request;
        ownerRequest.userId = owner.user.id;
        ownerRequest.loginHash = owner.user.loginHash;
        ownerRequest.approvalId.clear();
        check(sessions.create(ownerRequest).error() == SessionFailure::kApprovalRequired, "Owner obeys own grant policy");
        ownPolicy.verificationMethod = "direct";
        check(devices.setGrant(owner.user.id, device.id, ownPolicy, false), "Restore owner direct policy");
        phase = "disabled accounts and failure limits";
        check(identities.updateAccount(owner.user, guest.user.id, "user", true, {}), "Disable user");
        check(!identities.authenticate(guest.token) && !identities.login("guest", password, activated.recoveryCodes[1]), "Disabled user rejected");
        check(identities.createAccount(owner.user, "limited", password, "user"), "Rate test account");
        for (int i = 0; i < 5; ++i) {
            check(!identities.login("limited", "incorrect-password"), "Invalid login");
        }
        check(!identities.login("limited", password), "Account cooldown");
        phase = "administrator machine allocation";
        check(identities.createAccount(owner.user, "operator", password, "admin"), "Create allocation administrator");
        check(identities.createAccount(owner.user, "recipient", password, "user"), "Create allocation recipient");
        const auto operatorLogin = identities.login("operator", password).value();
        const auto recipient = identities.login("recipient", password).value();
        const auto privateDevice = devices.createDevice(recipient.user.id, "recipient machine");
        check(devices.list(operatorLogin.user.id).empty(), "Administrator receives no implicit desktop access");
        check(devices.find(operatorLogin.user.id, privateDevice.id, true).has_value(), "Administrator can discover another owner's machine");
        check(devices.list(recipient.user.id, true).empty() && !devices.find(recipient.user.id, device.id, true), "Ordinary user cannot request administrative scope");
        check(!devices.grants(recipient.user.id, device.id), "Ordinary user cannot read another owner's grants");
        DeviceGrant allocation{{}, "recipient", true, true, false, false, "direct"};
        check(devices.setGrant(operatorLogin.user.id, device.id, allocation, false), "Administrator assigns another owner's machine");
        check(devices.find(recipient.user.id, device.id)->writeControl, "Assigned user receives control");
        check(!devices.setGrant(recipient.user.id, device.id, allocation, false), "Recipient cannot delegate grants");
        DeviceGrant ownerGrant{{}, "owner", true, true, true, true, "direct"};
        check(!devices.setGrant(operatorLogin.user.id, device.id, ownerGrant, true), "Administrator cannot remove the owner's grant");
        check(!devices.canConfigureWindowsLogin(recipient.user.id, device.id), "Connection grant cannot configure credentials");
        check(!devices.canConfigureWindowsLogin(operatorLogin.user.id, device.id), "Non-owner administrator cannot configure credentials");
        check(devices.canConfigureWindowsLogin(owner.user.id, device.id), "Owner can configure credentials");
        allocation.manageCredentials = true;
        check(!devices.setGrant(operatorLogin.user.id, device.id, allocation, false), "Credential permission cannot be delegated");
        allocation.manageCredentials = false;
        allocation.verificationMethod = "telegram";
        check(devices.setGrant(operatorLogin.user.id, device.id, allocation, false), "Approval uses the device owner's Telegram binding");
        check(devices.setGrant(operatorLogin.user.id, device.id, allocation, true) && !devices.find(recipient.user.id, device.id), "Administrator revokes allocation");
        check(identities.updateAccount(owner.user, operatorLogin.user.id, "user", false, {}), "Demote administrator");
        check(!devices.setGrant(operatorLogin.user.id, device.id, allocation, false) && !devices.grants(operatorLogin.user.id, device.id), "Database role revocation is authoritative");
        std::cout << "Access integration passed: ownership, roles, grants, login-bound tickets, revocation, Telegram approval, TOTP and recovery\n";
        return 0;
    } catch (const DatabaseError& error) {
        std::cerr << "Access integration database error " << error.code() << " in phase: " << phase << '\n';
        return 1;
    } catch (const std::exception&) {
        std::cerr << "Access integration failed in phase: " << phase << '\n';
        return 1;
    }
}
