#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "modules/database/database.service.h"
#include "modules/devices/device.service.h"
#include "modules/identity/identity.service.h"
#include "modules/sessions/session.service.h"

namespace {
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Session invariant failed");
    }
}
} // namespace
int main() {
    using namespace ultraterimal;
    using namespace ultraterimal::server;
    const char* stage = "initialization";
    try {
        std::string database;
        std::getline(std::cin, database);
        require(!database.empty() && database.size() <= 4096);
        const std::string user = "u-" + randomToken(), other = "u-" + randomToken(), device = "d-" + randomToken();
        const std::string hash = tokenHash(randomToken());
        DeviceIdentity identity{device, hash};
        auto connection = openDatabase(database);
        {
            pqxx::work tx(connection);
            for (const auto& id : {user, other}) {
                tx.exec("INSERT INTO users(id,username,password_salt,password_hash,password_iterations) VALUES($1,$1,$2,$3,600000)", pqxx::params(tx, id, randomToken(), tokenHash(randomToken())));
            }
            tx.exec("INSERT INTO devices(id,name,credential_hash) VALUES($1,'Session fixture',$2)", pqxx::params(tx, device, hash));
            tx.exec("INSERT INTO device_grants(device_id,user_id) VALUES($1,$2)", pqxx::params(tx, device, user));
            tx.exec("INSERT INTO audit_events(user_id,device_id,event_type,success) VALUES($1,$2,'device.register',true)", pqxx::params(tx, user, device));
            tx.commit();
        }
        DeviceService deviceService(database);
        stage = "restart permission ownership and revocation";
        require(!deviceService.systemRestartPermission(other, device));
        require(deviceService.systemRestartPermission(user, device).value_or(false));
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE device_grants SET write_control=false WHERE device_id=$1", pqxx::params(tx, device));
            tx.commit();
        }
        require(deviceService.systemRestartPermission(user, device) == false);
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE device_grants SET write_control=true WHERE device_id=$1", pqxx::params(tx, device));
            tx.commit();
        }
        stage = "restart idempotency and conservative boot evidence";
        const auto restart = deviceService.createRestart(user, device, "restart-key", 1000);
        require(restart && restart->newlyCreated);
        const auto retried = deviceService.restartByIdempotencyKey(user, device, "restart-key");
        require(retried && retried->id == restart->id && !retried->newlyCreated);
        require(!deviceService.restartByIdempotencyKey(other, device, "restart-key"));
        require(!deviceService.createRestart(user, device, "restart-key", 2000)->newlyCreated);
        wire::WindowsRestartResult restartReply;
        restartReply.set_operation_id(restart->id);
        restartReply.set_status(wire::WindowsRestartResult::UNKNOWN);
        require(!deviceService.applyRestartResult({"other-device", hash}, restartReply));
        require(!deviceService.applyRestartResult({device, tokenHash(randomToken())}, restartReply));
        require(deviceService.restart(user, restart->id)->status == "PENDING");
        require(deviceService.applyRestartResult(identity, restartReply));
        deviceService.confirmRestart(device, 1010);
        require(deviceService.restart(user, restart->id)->status == "UNKNOWN");
        restartReply.set_status(wire::WindowsRestartResult::ACCEPTED);
        require(deviceService.applyRestartResult(identity, restartReply));
        require(!deviceService.applyRestartResult(identity, restartReply));
        deviceService.confirmRestart(device, 0);
        require(deviceService.restart(user, restart->id)->status == "ACCEPTED");
        deviceService.confirmRestart(device, 1003);
        require(deviceService.restart(user, restart->id)->status == "ACCEPTED");
        deviceService.confirmRestart(device, 1004);
        require(deviceService.restart(user, restart->id)->status == "ACCEPTED");
        SessionService service(database);
        const auto nextRestart = deviceService.createRestart(user, device, "restart-next-click", 1004);
        require(nextRestart && nextRestart->newlyCreated && nextRestart->id != restart->id);
        require(deviceService.restart(user, restart->id)->status == "ACCEPTED");
        SessionRequest request{user, device, wire::DESKTOP, true};
        request.desktopInputContract = 2;
        request.agentInputContractSupported = true;
        stage = "permission and device preemption";
        auto denied = request;
        denied.userId = other;
        const auto forbidden = service.create(denied);
        require(!forbidden && forbidden.error() == SessionFailure::kDenied);
        const auto desktop = service.create(request);
        require(desktop.has_value() && desktop->channels.size() == 2);
        stage = "desktop input upgrade admission";
        auto unsupported = request;
        unsupported.agentInputContractSupported = false;
        const auto incompatible = service.create(unsupported);
        require(!incompatible && incompatible.error() == SessionFailure::kIncompatible && service.authorized(desktop->id));
        unsupported = request;
        unsupported.desktopInputContract = 0;
        require(!service.create(unsupported) && service.authorized(desktop->id));
        unsupported.desktopInputContract = 1;
        const auto legacy = service.create(unsupported);
        require(!legacy && legacy.error() == SessionFailure::kInvalid && service.authorized(desktop->id));
        require(!service.get(other, desktop->id));
        require(service.get(user, desktop->id)->state == "pending");
        require(!service.closeForDevice({"wrong-device", hash}, desktop->id, SessionEnd::kFailure));
        service.disconnectDevice({device, tokenHash(randomToken())});
        require(service.authorized(desktop->id));
        request.kind = wire::VNC;
        request.desktopInputContract = 0;
        request.agentInputContractSupported = false;
        const auto replacementWriter = service.create(request);
        require(replacementWriter.has_value() && replacementWriter->preemptedSessions.size() == 1 && replacementWriter->preemptedSessions[0] == desktop->id);
        require(!service.authorized(desktop->id));
        request.writeControl = false;
        const auto viewer = service.create(request);
        require(viewer.has_value() && viewer->channels.size() == 1 && viewer->preemptedSessions.size() == 1 && viewer->preemptedSessions[0] == replacementWriter->id);
        auto recovery = request;
        recovery.preempt = false;
        const auto busyRecovery = service.create(recovery);
        require(!busyRecovery && busyRecovery.error() == SessionFailure::kBusy && service.authorized(viewer->id));
        auto sshRequest = request;
        sshRequest.kind = wire::SSH;
        require(!service.create(sshRequest));
        sshRequest.writeControl = true;
        const auto ssh = service.create(sshRequest);
        require(ssh.has_value() && ssh->channels.size() == 1 && ssh->channels[0].purpose == wire::SSH_STREAM);
        const auto sshChannel = service.consumeBrowser(user, ssh->id, ssh->channels[0].id, wire::SSH_STREAM, ssh->channels[0].browserTicket);
        require(sshChannel && sshChannel->userId == user && sshChannel->deviceId == device && sshChannel->kind == wire::SSH);
        require(service.close(user, ssh->id, SessionEnd::kUser));
        stage = "identity and purpose binding";
        const auto& first = desktop->channels[0];
        require(!service.consumeBrowser(other, desktop->id, first.id, first.purpose, first.browserTicket));
        require(!service.consumeBrowser(user, viewer->id, first.id, first.purpose, first.browserTicket));
        require(!service.consumeBrowser(user, desktop->id, first.id, wire::VNC_STREAM, first.browserTicket));
        require(!service.consumeAgent({device, tokenHash(randomToken())}, desktop->id, first.id, first.purpose, first.agentTicket));
        require(!service.consumeAgent({"wrong-device", hash}, desktop->id, first.id, first.purpose, first.agentTicket));
        require(!service.consumeBrowser(user, desktop->id, first.id, desktop->channels[1].purpose, first.browserTicket));
        require(!service.consumeAgent(identity, desktop->id, first.id, first.purpose, first.browserTicket));
        stage = "concurrent one-time consumption";
        auto nextDesktopRequest = SessionRequest{user, device, wire::DESKTOP, true};
        nextDesktopRequest.desktopInputContract = 2;
        nextDesktopRequest.agentInputContractSupported = true;
        const auto currentDesktop = service.create(nextDesktopRequest);
        require(currentDesktop.has_value() && currentDesktop->preemptedSessions.empty());
        const auto& currentFirst = currentDesktop->channels[0];
        auto consume = [&] { return service.consumeBrowser(user, currentDesktop->id, currentFirst.id, currentFirst.purpose, currentFirst.browserTicket).has_value(); };
        auto one = std::async(std::launch::async, consume);
        auto two = std::async(std::launch::async, consume);
        require(static_cast<int>(one.get()) + static_cast<int>(two.get()) == 1);
        require(service.consumeAgent(identity, currentDesktop->id, currentFirst.id, currentFirst.purpose, currentFirst.agentTicket).has_value());
        const auto& second = currentDesktop->channels[1];
        require(service.consumeBrowser(user, currentDesktop->id, second.id, second.purpose, second.browserTicket).has_value());
        require(service.consumeAgent(identity, currentDesktop->id, second.id, second.purpose, second.agentTicket).has_value());
        {
            pqxx::read_transaction tx(connection);
            require(tx.exec("SELECT state FROM remote_sessions WHERE id=$1", pqxx::params(tx, currentDesktop->id))[0][0].as<std::string>() == "active");
            const auto hashes = tx.exec("SELECT browser_ticket_hash,agent_ticket_hash FROM session_channels WHERE id=$1", pqxx::params(tx, currentFirst.id));
            require(hashes[0][0].as<std::string>() == tokenHash(currentFirst.browserTicket) && hashes[0][1].as<std::string>() == tokenHash(currentFirst.agentTicket));
        }
        stage = "permission withdrawal and close";
        require(service.authorized(currentDesktop->id));
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE device_grants SET write_control=false WHERE device_id=$1 AND user_id=$2", pqxx::params(tx, device, user));
            tx.commit();
        }
        require(!service.authorized(currentDesktop->id));
        require(!service.close(other, currentDesktop->id, SessionEnd::kUser));
        require(service.close(user, currentDesktop->id, SessionEnd::kPermissionRevoked));
        require(service.close(user, currentDesktop->id, SessionEnd::kPermissionRevoked));
        require(!service.authorized(currentDesktop->id));
        require(service.close(user, viewer->id, SessionEnd::kUser));
        require(!service.consumeBrowser(user, viewer->id, viewer->channels[0].id, wire::VNC_STREAM, viewer->channels[0].browserTicket));
        {
            pqxx::work tx(connection);
            require(tx.exec("SELECT count(*) FROM audit_events WHERE session_id=$1 AND event_type='session.end'", pqxx::params(tx, currentDesktop->id))[0][0].as<int>() == 1);
            tx.exec("UPDATE device_grants SET write_control=true WHERE device_id=$1 AND user_id=$2", pqxx::params(tx, device, user));
            tx.commit();
        }
        stage = "expiry and expired writer release";
        request.kind = wire::DESKTOP;
        request.writeControl = true;
        request.desktopInputContract = 2;
        request.agentInputContractSupported = true;
        const auto expired = service.create(request);
        require(expired.has_value());
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE remote_sessions SET setup_expires_at=now()-interval '1 second' WHERE id=$1", pqxx::params(tx, expired->id));
            tx.commit();
        }
        require(!service.authorized(expired->id));
        const auto& expiredChannel = expired->channels[0];
        require(!service.consumeBrowser(user, expired->id, expiredChannel.id, expiredChannel.purpose, expiredChannel.browserTicket));
        const auto replacement = service.create(request);
        require(replacement.has_value());
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE session_channels SET expires_at=now()-interval '1 second' WHERE session_id=$1", pqxx::params(tx, replacement->id));
            tx.commit();
        }
        require(!service.consumeAgent(identity, replacement->id, replacement->channels[0].id, replacement->channels[0].purpose, replacement->channels[0].agentTicket));
        require(service.close(user, replacement->id, SessionEnd::kUser));
        stage = "concurrent device preemption";
        auto firstWriter = std::async(std::launch::async, [&] { return service.create(request); });
        auto secondWriter = std::async(std::launch::async, [&] { auto vnc = request; vnc.kind = wire::VNC; vnc.desktopInputContract = 0; return service.create(vnc); });
        auto writerOne = firstWriter.get();
        auto writerTwo = secondWriter.get();
        require(writerOne.has_value() && writerTwo.has_value());
        require(writerOne->preemptedSessions.size() + writerTwo->preemptedSessions.size() >= 1);
        const auto remaining = service.authorized(writerOne->id) ? writerOne->id : writerTwo->id;
        require(service.authorized(remaining));
        require(service.close(user, remaining, SessionEnd::kUser));
        stage = "cross-device user count bound";
        std::vector<SessionRequest> deviceRequests;
        {
            pqxx::work tx(connection);
            for (int i = 0; i < 17; ++i) {
                auto additional = request;
                additional.deviceId = "d-" + randomToken();
                deviceRequests.push_back(additional);
                const auto& id = deviceRequests.back().deviceId;
                tx.exec("INSERT INTO devices(id,name,credential_hash) VALUES($1,'Session limit fixture',$2)", pqxx::params(tx, id, tokenHash(randomToken())));
                tx.exec("INSERT INTO device_grants(device_id,user_id) VALUES($1,$2)", pqxx::params(tx, id, user));
            }
            tx.commit();
        }
        for (int i = 0; i < 16; ++i) {
            require(service.create(deviceRequests[i]).has_value());
        }
        const auto userLimited = service.create(deviceRequests[16]);
        require(!userLimited && userLimited.error() == SessionFailure::kLimit);
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE remote_sessions SET setup_expires_at=now()-interval '1 second' WHERE user_id=$1 AND state='pending'", pqxx::params(tx, user));
            tx.commit();
        }
        require(service.create(deviceRequests[16]).has_value());
        stage = "revoked device";
        {
            pqxx::work tx(connection);
            tx.exec("UPDATE devices SET revoked_at=now() WHERE id=$1", pqxx::params(tx, device));
            tx.commit();
        }
        const auto revoked = service.create(request);
        require(!revoked && revoked.error() == SessionFailure::kDenied);
        {
            pqxx::read_transaction tx(connection);
            require(tx.exec("SELECT count(*) FROM audit_events WHERE user_id=$1 AND event_type='session.create' AND NOT success AND reason='session_limit'", pqxx::params(tx, user))[0][0].as<int>() == 1);
        }
        stage = "device names, unique credentials and deletion";
        const auto managedPending = deviceService.createDevice(other, "Original");
        const auto independentPending = deviceService.createDevice(other, "Independent");
        const auto managed = deviceService.registerDevice(*deviceService.createEnrollment(other, managedPending.id), "Original");
        const auto independent = deviceService.registerDevice(*deviceService.createEnrollment(other, independentPending.id), "Independent");
        require(managed && independent && managed->credential != independent->credential && managed->deviceId != independent->deviceId);
        require(deviceService.authenticate(managed->credential)->id == managed->deviceId);
        require(!deviceService.rename(user, managed->deviceId, "Forbidden"));
        require(!deviceService.rename(other, managed->deviceId, "   "));
        require(deviceService.rename(other, managed->deviceId, "Renamed"));
        require(deviceService.find(other, managed->deviceId)->name == "Renamed");
        require(deviceService.authenticate(managed->credential)->id == managed->deviceId);
        auto managedRequest = request;
        managedRequest.userId = other;
        managedRequest.deviceId = managed->deviceId;
        const auto managedSession = service.create(managedRequest);
        require(managedSession.has_value());
        require(!deviceService.remove(user, managed->deviceId));
        require(deviceService.remove(other, managed->deviceId));
        require(!deviceService.authenticate(managed->credential));
        require(!deviceService.find(other, managed->deviceId));
        require(!service.authorized(managedSession->id));
        require(service.revokeDevice(managed->deviceId).size() == 1);
        require(service.get(other, managedSession->id)->state == "closed");
        require(deviceService.authenticate(independent->credential).has_value());
        require(deviceService.remove(other, managed->deviceId));
        std::cout << "Session grants, purpose binding, one-time tickets, device preemption, limits, revocation and audit passed\n";
        return 0;
    } catch (...) {
        std::cerr << "Session test failed at " << stage << '\n';
        return 1;
    }
}
