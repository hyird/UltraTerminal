#include "device.controller.h"

#include <spdlog/spdlog.h>

#include "config/request.schema.h"
#include "installer.script.h"
#include "middleware/json.transport.h"
#include "update.service.h"
namespace ultraterimal::server {
ruvia::Task<ruvia::HttpResponse> DeviceController::update(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"device_id"});
    const auto device = requestText(body, "device_id", 128);
    auto& runtime = context.workerState<ServerRuntime>();
    const auto user = context.requestState<UserIdentity>().id;
    const auto allowed = co_await context.runBlocking([service = runtime.devices, device, user] {
        const auto devices = service.list(user);
        return std::any_of(devices.begin(), devices.end(), [&](const auto& found) { return found.id == device && found.canManage; });
    });
    if (!allowed) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Only the device registrant can update its Agent"});
    }
    const auto release = co_await context.runBlocking([root = runtime.config.webRoot] { return agentRelease(root); });
    if (!release) {
        co_return context.error({.status = ruvia::http_status::kConflict, .code = "release_unavailable", .message = "No validated Agent release is published"});
    }
    const auto found = runtime.controlConnections.find(device);
    if (found == runtime.controlConnections.end() || !found->second->ready || !found->second->capabilities.agent_update()) {
        co_return context.error({.status = ruvia::http_status::kConflict, .code = "agent_unavailable", .message = "Agent is offline or requires an initial installer upgrade"});
    }
    const auto connection = found->second;
    if (!protocol::newerUpdate(release->update, connection->agentVersion)) {
        co_return jsonResponse(context, {{"status", "current"}, {"version", connection->agentVersion}});
    }
    if (!connection->updatePending) {
        wire::Envelope command;
        *command.mutable_agent_update() = release->update;
        if (!connection->enqueue(std::move(command))) {
            co_return context.error({.status = ruvia::http_status::kServiceUnavailable, .code = "agent_busy", .message = "Agent control queue is unavailable"});
        }
        connection->updatePending = true;
        spdlog::info("agent_update_requested user_id={} device_id={} version={}", user, device, release->update.version());
    }
    context.status(ruvia::http_status::kAccepted);
    co_return jsonResponse(context, {{"status", "requested"}, {"version", release->update.version()}});
}
ruvia::Task<ruvia::HttpResponse> DeviceInstallerController::script(ruvia::Context& context) {
    context.header("Cache-Control", "no-store");
    context.header("X-Content-Type-Options", "nosniff");
    co_return context.text(kWindowsInstallScript);
}
ruvia::Task<ruvia::HttpResponse> DeviceController::install(ruvia::Context& context) {
    context.header("Cache-Control", "no-store");
    context.header("Referrer-Policy", "no-referrer");
    context.header("X-Content-Type-Options", "nosniff");
    const auto code = std::string(context.req().query("code").value_or(""));
    const auto repair = std::string(context.req().query("device").value_or(""));
    std::optional<std::string> device;
    if (!code.empty() && repair.empty()) {
        device = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, code] { return service.enrollmentDevice(code); });
    } else if (code.empty() && repair.starts_with("d-") && validToken(std::string_view(repair).substr(2))) {
        // Repair carries no enrollment capability; the installer requires this
        // exact identity and its existing DPAPI credential on the local machine.
        device = repair;
    }
    if (!device) {
        context.status(ruvia::http_status::kNotFound);
        co_return context.text("throw 'Installation command expired or invalid. Copy a new command from device management.'\n");
    }
    const auto& origin = context.workerState<ServerRuntime>().config.origin;
    const auto script = "& { $utOrigin='" + origin + "'; $utDevice='" + *device + "'; $utCode='" + code + "'; $ErrorActionPreference='Stop'; irm ($utOrigin + '/install.ps1') | iex }\n";
    co_return context.text(std::pmr::string(script, context.arena()));
}
ruvia::Task<ruvia::HttpResponse> DeviceController::rename(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "name"});
    const auto device = requestText(body, "device_id", 128);
    const auto name = requestText(body, "name", 128);
    const bool updated = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, user = context.requestState<UserIdentity>().id, device, name] { return service.rename(user, device, name); });
    if (!updated) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Only the device registrant can rename an active device"});
    }
    co_return jsonResponse(context, {{"id", device}, {"name", name}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::remove(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"device_id"});
    const auto device = requestText(body, "device_id", 128);
    auto& runtime = context.workerState<ServerRuntime>();
    const bool removed = co_await context.runBlocking([service = runtime.devices, user = context.requestState<UserIdentity>().id, device] { return service.remove(user, device); });
    if (!removed) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Only the device registrant can delete a device"});
    }
    const auto control = runtime.controlConnections.find(device);
    if (control != runtime.controlConnections.end()) {
        control->second->close();
    }
    const auto sessions = co_await context.runBlocking([service = runtime.sessions, device] { return service.revokeDevice(device); });
    for (const auto& id : sessions) {
        const auto session = runtime.desktopSessions.find(id);
        if (session != runtime.desktopSessions.end()) {
            session->second->close(DesktopCloseReason::kDeviceDeleted);
        }
    }
    co_return jsonResponse(context, {{"id", device}, {"deleted", true}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::windowsLogin(ruvia::Context& context) {
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
        co_return context.error({.status = ruvia::http_status::kBadRequest, .code = "invalid_field", .message = "Invalid Windows credential operation"});
    }
    body.clear();
    auto& runtime = context.workerState<ServerRuntime>();
    const bool allowed = co_await context.runBlocking([service = runtime.devices, user = context.requestState<UserIdentity>().id, device] { return service.canConfigureWindowsLogin(user, device); });
    if (!allowed) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Windows credential configuration requires control permission"});
    }
    const auto found = runtime.controlConnections.find(device);
    if (found == runtime.controlConnections.end() || !found->second->ready || !found->second->capabilities.windows_login_settings()) {
        co_return context.error({.status = ruvia::http_status::kConflict, .code = "agent_unavailable", .message = "Agent is offline or does not support Windows credentials"});
    }
    const auto connection = found->second;
    auto result = co_await connection->windowsLogin(std::move(request));
    co_await context.runBlocking([service = runtime.devices, user = context.requestState<UserIdentity>().id, device, action, success = result && result->success()] { service.auditWindowsLogin(user, device, action, success); });
    if (!result || !result->success()) {
        co_return context.error({.status = ruvia::http_status::kServiceUnavailable, .code = "agent_settings_failed", .message = "Agent could not complete the credential operation; read the saved state before retrying"});
    }
    co_return jsonResponse(context, {{"saved", result->saved()}, {"username", result->username()}, {"domain", result->domain()}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::restartPermissionStatus(ruvia::Context& context) {
    const auto device = std::string(context.req().query("deviceId").value_or(""));
    if (device.empty() || device.size() > 128) {
        co_return context.error({.status = ruvia::http_status::kBadRequest, .code = "invalid_field", .message = "deviceId is required"});
    }
    const auto allowed = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, user = context.requestState<UserIdentity>().id, device] { return service.systemRestartPermission(user, device); });
    if (!allowed) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Only the device registrant can read restart permission"});
    }
    co_return jsonResponse(context, {{"allowed", *allowed}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::restart(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"deviceId", "idempotencyKey"});
    const auto device = requestText(body, "deviceId", 128);
    const auto key = requestText(body, "idempotencyKey", 128);
    auto& runtime = context.workerState<ServerRuntime>();
    const auto user = context.requestState<UserIdentity>().id;
    auto operation = co_await context.runBlocking([service = runtime.devices, user, device, key] { return service.restartByIdempotencyKey(user, device, key); });
    std::shared_ptr<ControlConnection> control;
    if (!operation) {
        const bool allowed = co_await context.runBlocking([service = runtime.devices, user, device] { const auto permission = service.systemRestartPermission(user, device); return permission && *permission; });
        if (!allowed) {
            co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Restart is not permitted"});
        }
        const auto current = runtime.controlConnections.find(device);
        if (current == runtime.controlConnections.end() || !current->second->ready || !current->second->capabilities.system_restart()) {
            co_return context.error({.status = ruvia::http_status::kConflict, .code = "agent_unavailable", .message = "Device is offline or does not support restart"});
        }
        control = current->second;
        operation = co_await context.runBlocking([service = runtime.devices, user, device, key, boot = control->capabilities.boot_time_unix_seconds()] { return service.createRestart(user, device, key, boot); });
    }
    if (!operation) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Restart is not permitted"});
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
            co_await context.runBlocking([service = runtime.devices, id = operation->id] { service.updateRestart(id, "FAILED"); });
            operation->status = "FAILED";
        }
    }
    context.status(ruvia::http_status::kAccepted);
    const auto state = operation->status == "RESOLVED" ? "resolved" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                  : operation->status == "UNKNOWN"    ? "unknown"
                                                                  : operation->status == "FAILED"     ? "failed"
                                                                  : operation->status == "COMPLETED"  ? "confirmed"
                                                                                                      : "requested";
    co_return jsonResponse(context, {{"id", operation->id}, {"status", state}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::restartStatus(ruvia::Context& context) {
    const auto id = std::string(context.req().param("id").value_or(""));
    const auto operation = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, user = context.requestState<UserIdentity>().id, id] { return service.restart(user, id); });
    if (!operation) {
        co_return context.error({.status = ruvia::http_status::kNotFound, .code = "not_found", .message = "Restart operation not found"});
    }
    const auto status = operation->status == "PENDING" ? "requested" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                   : operation->status == "COMPLETED"  ? "confirmed"
                                                                   : operation->status == "FAILED"     ? "failed"
                                                                   : operation->status == "RESOLVED"   ? "resolved"
                                                                                                       : "unknown";
    co_return jsonResponse(context, {{"id", operation->id}, {"status", status}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::enrollment(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "platform"});
    const auto id = requestText(body, "device_id", 128);
    if (requestText(body, "platform", 32) != "windows-x64") {
        co_return context.error({.status = ruvia::http_status::kBadRequest, .code = "unsupported_platform", .message = "Platform not supported"});
    }
    const auto code = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, userId = context.requestState<UserIdentity>().id, id] { return service.createEnrollment(userId, id); });
    if (!code) {
        co_return context.error({.status = ruvia::http_status::kForbidden, .code = "access_denied", .message = "Only the device owner can install an Agent"});
    }
    context.status(ruvia::http_status::kCreated);
    const auto& config = context.workerState<ServerRuntime>().config;
    co_return jsonResponse(context, {{"code", *code}, {"device_id", id}, {"expires_in", code->empty() ? 0 : 600}, {"web_origin", config.origin}, {"native_host", config.nativeHost}, {"native_address", config.nativePublicAddress}, {"native_port", config.nativePort}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::create(ruvia::Context& context) {
    const auto body = requestObject(co_await context.req().text(), {"name"});
    const auto name = requestText(body, "name", 128);
    const auto device = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, userId = context.requestState<UserIdentity>().id, name] { return service.createDevice(userId, name); });
    context.status(ruvia::http_status::kCreated);
    co_return jsonResponse(context, {{"id", device.id}, {"name", device.name}, {"enrolled", false}, {"can_manage", true}, {"online", false}, {"last_seen", nullptr}});
}
ruvia::Task<ruvia::HttpResponse> DeviceController::devices(ruvia::Context& context) {
    const auto devices = co_await context.runBlocking([service = context.workerState<ServerRuntime>().devices, userId = context.requestState<UserIdentity>().id] { return service.list(userId); });
    boost::json::array response;
    for (const auto& device : devices) {
        const auto& connections = context.workerState<ServerRuntime>().controlConnections;
        const auto connection = connections.find(device.id);
        boost::json::object item{{"id", device.id}, {"name", device.name}, {"enrolled", device.enrolled}, {"can_manage", device.canManage}, {"online", connection != connections.end() && connection->second->ready}};
        item["last_seen"] = device.lastSeen ? boost::json::value(*device.lastSeen) : boost::json::value(nullptr);
        response.push_back(std::move(item));
    }
    co_return jsonResponse(context, response);
}
} // namespace ultraterimal::server
