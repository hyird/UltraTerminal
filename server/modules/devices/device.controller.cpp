#include "device.controller.h"

#include <spdlog/spdlog.h>

#include "config/request.schema.h"
#include "features/desktop/access.runtime.h"
#include "installer.script.h"
#include "middleware/json.transport.h"
#include "update.service.h"
namespace ultraterimal::server {
ruvia::task<ruvia::http_response> DeviceController::update(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"device_id"});
    const auto device = requestText(body, "device_id", 128);
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const auto user = context.request_state<UserIdentity>().id;
    const auto allowed = (co_await context.run_blocking([service = runtime.devices, device, user] {
                             const auto devices = service.list(user);
                             return std::any_of(devices.begin(), devices.end(), [&](const auto& found) { return found.id == device && found.canManage; });
                         })).value();
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can update its Agent"});
    }
    const auto release = (co_await context.run_blocking([root = runtime.config.webRoot] { return agentRelease(root); })).value();
    if (!release) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "No validated Agent release is published"});
    }
    const auto found = runtime.controlConnections.find(device);
    if (found == runtime.controlConnections.end() || !found->second->ready || !found->second->capabilities.agent_update()) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Agent is offline or requires an initial installer upgrade"});
    }
    const auto connection = found->second;
    if (!protocol::newerUpdate(release->update, connection->agentVersion)) {
        co_return jsonResponse(context, {{"status", "current"}, {"version", connection->agentVersion}});
    }
    if (!connection->updatePending) {
        wire::Envelope command;
        *command.mutable_agent_update() = release->update;
        if (!connection->enqueue(std::move(command))) {
            co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_busy", .message_ = "Agent control queue is unavailable"});
        }
        connection->updatePending = true;
        spdlog::info("agent_update_requested user_id={} device_id={} version={}", user, device, release->update.version());
    }
    context.status(ruvia::http_status::accepted);
    co_return jsonResponse(context, {{"status", "requested"}, {"version", release->update.version()}});
}
ruvia::task<ruvia::http_response> DeviceInstallerController::script(ruvia::context& context) const {
    context.header("Cache-Control", "no-store");
    context.header("X-Content-Type-Options", "nosniff");
    co_return context.text(kWindowsInstallScript);
}
ruvia::task<ruvia::http_response> DeviceController::install(ruvia::context& context) const {
    context.header("Cache-Control", "no-store");
    context.header("Referrer-Policy", "no-referrer");
    context.header("X-Content-Type-Options", "nosniff");
    const auto code = std::string(context.req().query("code").value_or(""));
    const auto repair = std::string(context.req().query("device").value_or(""));
    std::optional<std::string> device;
    if (!code.empty() && repair.empty()) {
        device = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, code] { return service.enrollmentDevice(code); })).value();
    } else if (code.empty() && repair.starts_with("d-") && validToken(std::string_view(repair).substr(2))) {
        // Repair carries no enrollment capability; the installer requires this
        // exact identity and its existing DPAPI credential on the local machine.
        device = repair;
    }
    if (!device) {
        context.status(ruvia::http_status::not_found);
        co_return context.text("throw 'Installation command expired or invalid. Copy a new command from device management.'\n");
    }
    const auto& origin = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).config.origin;
    const auto script = "& { $utOrigin='" + origin + "'; $utDevice='" + *device + "'; $utCode='" + code + "'; $ErrorActionPreference='Stop'; irm ($utOrigin + '/install.ps1') | iex }\n";
    co_return context.text(std::pmr::string(script, context.arena()));
}
ruvia::task<ruvia::http_response> DeviceController::rename(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "name"});
    const auto device = requestText(body, "device_id", 128);
    const auto name = requestText(body, "name", 128);
    const bool updated = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, user = context.request_state<UserIdentity>().id, device, name] { return service.rename(user, device, name); })).value();
    if (!updated) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can rename an active device"});
    }
    (*context.connection_state<std::shared_ptr<ServerRuntime>>()).status.publish("d:" + device);
    co_return jsonResponse(context, {{"id", device}, {"name", name}});
}
ruvia::task<ruvia::http_response> DeviceController::transferOwner(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "owner_id", "current_password", "code", "telegram_proof"});
    const auto device = requestText(body, "device_id", 128);
    const auto owner = requestText(body, "owner_id", 128);
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const auto user = context.request_state<UserIdentity>();
    const bool transferred = (co_await context.run_blocking([identities = runtime.identities, devices = runtime.devices, user, device, owner, password = optionalText(body, "current_password", 1024), code = optionalText(body, "code", 32), telegramProof = optionalText(body, "telegram_proof", 32)] {
                                 return user.role == "admin" && identities.reauthenticate(user, password, code, telegramProof) && devices.transferOwner(user.id, device, owner);
                             })).value();
    if (!transferred) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "owner_transfer_rejected", .message_ = "Owner transfer rejected"});
    }
    co_await revalidateAccess(context);
    runtime.status.publish("d:" + device);
    runtime.status.publish("u:*");
    co_return jsonResponse(context, {{"id", device}, {"owner_id", owner}});
}
ruvia::task<ruvia::http_response> DeviceController::remove(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"device_id"});
    const auto device = requestText(body, "device_id", 128);
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const bool removed = (co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device] { return service.remove(user, device); })).value();
    if (!removed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can delete a device"});
    }
    const auto control = runtime.controlConnections.find(device);
    if (control != runtime.controlConnections.end()) {
        control->second->close();
    }
    const auto sessions = (co_await context.run_blocking([service = runtime.sessions, device] { return service.revokeDevice(device); })).value();
    for (const auto& id : sessions) {
        const auto session = runtime.desktopSessions.find(id);
        if (session != runtime.desktopSessions.end()) {
            session->second->close(DesktopCloseReason::kDeviceDeleted);
        }
    }
    runtime.status.publish("d:" + device);
    runtime.status.publish("u:*");
    co_return jsonResponse(context, {{"id", device}, {"deleted", true}});
}
ruvia::task<ruvia::http_response> DeviceController::windowsLogin(ruvia::context& context) const {
    auto body = requestObject(co_await context.req().text(), {"device_id", "action", "username", "domain", "password"});
    const auto device = requestText(body, "device_id", 128);
    const auto action = requestText(body, "action", 8);
    wire::WindowsLoginRequest request;
    request.set_action(action == "read" ? wire::WindowsLoginRequest::READ : action == "save" ? wire::WindowsLoginRequest::SAVE
                                                                        : action == "clear"  ? wire::WindowsLoginRequest::CLEAR
                                                                                             : wire::WindowsLoginRequest::UNSPECIFIED);
    if (request.action() == wire::WindowsLoginRequest::SAVE) {
        const auto* username = body.if_contains("username");
        if (username && (!username->is_string() || !username->as_string().empty())) {
            request.set_username(requestText(body, "username", 512));
        }
        const auto* password = body.if_contains("password");
        if (password && (!password->is_string() || !password->as_string().empty())) {
            request.set_password(requestText(body, "password", 1024));
        }
        const auto* domain = body.if_contains("domain");
        if (domain && (!domain->is_string() || !domain->as_string().empty())) {
            request.set_domain(requestText(body, "domain", 256));
        }
    } else if (body.if_contains("username") || body.if_contains("domain") || body.if_contains("password") || request.action() == wire::WindowsLoginRequest::UNSPECIFIED) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Invalid Windows credential operation"});
    }
    body.clear();
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const bool allowed = (co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device] { return service.canConfigureWindowsLogin(user, device); })).value();
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Windows credential configuration requires control permission"});
    }
    const auto found = runtime.controlConnections.find(device);
    if (found == runtime.controlConnections.end() || !found->second->ready || !found->second->capabilities.windows_login_settings()) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Agent is offline or does not support Windows credentials"});
    }
    const auto connection = found->second;
    auto result = co_await connection->windowsLogin(std::move(request));
    (co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device, action, success = result && result->success()] { service.auditWindowsLogin(user, device, action, success); })).value();
    if (!result || !result->success()) {
        co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_settings_failed", .message_ = "Agent could not complete the credential operation; read the saved state before retrying"});
    }
    co_return jsonResponse(context, {{"saved", result->saved()}, {"username", result->username()}, {"domain", result->domain()}});
}
ruvia::task<ruvia::http_response> DeviceController::restartPermissionStatus(ruvia::context& context) const {
    const auto device = std::string(context.req().query("deviceId").value_or(""));
    if (device.empty() || device.size() > 128) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "deviceId is required"});
    }
    const auto allowed = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, user = context.request_state<UserIdentity>().id, device] { return service.systemRestartPermission(user, device); })).value();
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can read restart permission"});
    }
    co_return jsonResponse(context, {{"allowed", *allowed}});
}
ruvia::task<ruvia::http_response> DeviceController::restart(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"deviceId", "idempotencyKey"});
    const auto device = requestText(body, "deviceId", 128);
    const auto key = requestText(body, "idempotencyKey", 128);
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const auto user = context.request_state<UserIdentity>().id;
    auto operation = (co_await context.run_blocking([service = runtime.devices, user, device, key] { return service.restartByIdempotencyKey(user, device, key); })).value();
    std::shared_ptr<ControlConnection> control;
    if (!operation) {
        const bool allowed = (co_await context.run_blocking([service = runtime.devices, user, device] { const auto permission = service.systemRestartPermission(user, device); return permission && *permission; })).value();
        if (!allowed) {
            co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Restart is not permitted"});
        }
        const auto current = runtime.controlConnections.find(device);
        if (current == runtime.controlConnections.end() || !current->second->ready || !current->second->capabilities.system_restart()) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Device is offline or does not support restart"});
        }
        control = current->second;
        operation = (co_await context.run_blocking([service = runtime.devices, user, device, key, boot = control->capabilities.boot_time_unix_seconds()] { return service.createRestart(user, device, key, boot); })).value();
    }
    if (!operation) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Restart is not permitted"});
    }
    if (operation->newlyCreated && control) {
        const auto current = runtime.controlConnections.find(device);
        wire::WindowsRestartRequest request;
        request.set_operation_id(operation->id);
        request.set_device_id(device);
        request.set_expires_at_unix_seconds(operation->expiresAtUnixSeconds);
        // This synchronous handoff is the ownership boundary: HTTP never waits for
        // the Agent. Cancellation before it leaves the durable PENDING record to expire.
        const bool enqueued = control->ready && current != runtime.controlConnections.end() && current->second == control && control->enqueueRestart(std::move(request));
        if (!enqueued) {
            (co_await context.run_blocking([service = runtime.devices, id = operation->id] { service.updateRestart(id, "FAILED"); })).value();
            operation->status = "FAILED";
        }
    }
    context.status(ruvia::http_status::accepted);
    const auto state = operation->status == "RESOLVED" ? "resolved" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                  : operation->status == "UNKNOWN"    ? "unknown"
                                                                  : operation->status == "FAILED"     ? "failed"
                                                                  : operation->status == "COMPLETED"  ? "confirmed"
                                                                                                      : "requested";
    co_return jsonResponse(context, {{"id", operation->id}, {"status", state}});
}
ruvia::task<ruvia::http_response> DeviceController::restartStatus(ruvia::context& context) const {
    const auto id = std::string(context.req().param("id").value_or(""));
    const auto operation = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, user = context.request_state<UserIdentity>().id, id] { return service.restart(user, id); })).value();
    if (!operation) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Restart operation not found"});
    }
    const auto status = operation->status == "PENDING" ? "requested" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                   : operation->status == "COMPLETED"  ? "confirmed"
                                                                   : operation->status == "FAILED"     ? "failed"
                                                                   : operation->status == "RESOLVED"   ? "resolved"
                                                                                                       : "unknown";
    co_return jsonResponse(context, {{"id", operation->id}, {"status", status}});
}
ruvia::task<ruvia::http_response> DeviceController::enrollment(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "platform"});
    const auto id = requestText(body, "device_id", 128);
    if (requestText(body, "platform", 32) != "windows-x64") {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "unsupported_platform", .message_ = "Platform not supported"});
    }
    const auto code = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, userId = context.request_state<UserIdentity>().id, id] { return service.createEnrollment(userId, id); })).value();
    if (!code) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device owner can install an Agent"});
    }
    context.status(ruvia::http_status::created);
    const auto& config = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).config;
    co_return jsonResponse(context, {{"code", *code}, {"device_id", id}, {"expires_in", code->empty() ? 0 : 600}, {"web_origin", config.origin}, {"native_host", config.nativeHost}, {"native_address", config.nativePublicAddress}, {"native_port", config.nativePort}});
}
ruvia::task<ruvia::http_response> DeviceController::create(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"name"});
    const auto name = requestText(body, "name", 128);
    const auto device = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, userId = context.request_state<UserIdentity>().id, name] { return service.createDevice(userId, name); })).value();
    context.status(ruvia::http_status::created);
    (*context.connection_state<std::shared_ptr<ServerRuntime>>()).status.publish("d:" + device.id);
    co_return jsonResponse(context, {{"id", device.id}, {"name", device.name}, {"enrolled", false}, {"can_manage", true}, {"desktop", true}, {"write_control", true}, {"verification_method", "direct"}, {"manage_credentials", true}, {"online", false}, {"last_seen", nullptr}});
}
ruvia::task<ruvia::http_response> DeviceController::devices(ruvia::context& context) const {
    const auto devices = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, userId = context.request_state<UserIdentity>().id] { return service.list(userId); })).value();
    boost::json::array response;
    for (const auto& device : devices) {
        const auto& connections = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).controlConnections;
        const auto connection = connections.find(device.id);
        boost::json::object item{{"agent_version", connection != connections.end() && connection->second->ready ? boost::json::value(connection->second->agentVersion) : boost::json::value(nullptr)}, {"id", device.id}, {"name", device.name}, {"enrolled", device.enrolled}, {"can_manage", device.canManage}, {"desktop", device.desktop}, {"write_control", device.writeControl}, {"verification_method", device.verificationMethod}, {"manage_credentials", device.manageCredentials}, {"online", connection != connections.end() && connection->second->ready}};
        item["last_seen"] = device.lastSeen ? boost::json::value(*device.lastSeen) : boost::json::value(nullptr);
        response.push_back(std::move(item));
    }
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::grants(ruvia::context& context) const {
    const auto result = (co_await context.run_blocking([service = (*context.connection_state<std::shared_ptr<ServerRuntime>>()).devices, user = context.request_state<UserIdentity>().id, device = std::string(context.req().param("id").value_or(""))] { return service.grants(user, device); })).value();
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Device owner or administrator required"});
    }
    boost::json::array grants;
    for (const auto& g : *result) {
        grants.push_back(boost::json::object{{"user_id", g.userId}, {"username", g.username}, {"connect", g.desktop && g.writeControl}, {"verification_method", g.verificationMethod}});
    }
    co_return jsonResponse(context, grants);
}
ruvia::task<ruvia::http_response> DeviceController::saveGrant(ruvia::context& context) const {
    const auto body = requestObject(co_await context.req().text(), {"username", "connect", "verification_method", "remove"});
    const auto device = std::string(context.req().param("id").value_or(""));
    const DeviceGrant grant{{}, requestText(body, "username", 64), requestBool(body, "connect"), requestBool(body, "connect"), false, false, requestText(body, "verification_method", 16)};
    auto& runtime = (*context.connection_state<std::shared_ptr<ServerRuntime>>());
    const bool ok = (co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device, grant, remove = requestBool(body, "remove")] { return service.setGrant(user, device, grant, remove); })).value();
    if (!ok) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "grant_rejected", .message_ = "Grant rejected; check administrator or owner, target user and Telegram configuration"});
    }
    co_await revalidateAccess(context);
    runtime.status.publish("d:" + device);
    runtime.status.publish("u:*");
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
