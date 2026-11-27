#include "device.controller.h"

#include <algorithm>
#include <exception>
#include <typeinfo>
#include <utility>

#include <spdlog/spdlog.h>

#include "features/desktop/access.runtime.h"
#include "installer.script.h"
#include "update.service.h"

namespace ultraterimal::server {
namespace {
ruvia::task<bool> authorizeInstallerRequest(const DeviceService& service, ruvia::db_executor db, std::string deviceId, std::string code, std::string credential) {
    if (!code.empty() && credential.empty()) {
        const auto enrolled = co_await service.enrollmentDevice(db, code);
        co_return enrolled && *enrolled == deviceId;
    }
    if (code.empty() && !credential.empty()) {
        const auto identity = co_await service.authenticate(db, credential);
        co_return identity && identity->id == deviceId;
    }
    co_return false;
}
} // namespace
ruvia::task<ruvia::http_response> DeviceController::update(ruvia::context& context, const DeviceIdRequest& body) {
    std::string_view stage = "ownership_query";
    try {
        const auto device = textValue(body.get<"device_id">());
        auto& runtime = context.worker_state<ServerRuntime>();
        const auto user = context.request_state<UserIdentity>().id;
        const auto devices = co_await runtime.devices.list(context.db(), user);
        const bool allowed = std::any_of(devices.begin(), devices.end(), [&](const auto& found) { return found.id == device && found.canManage; });
        if (!allowed) {
            co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can update its Agent"});
        }
        stage = "release_lookup";
        const auto release = ((co_await context.run_blocking([root = runtime.config.agentReleaseDir] { return agentRelease(root); })).take_value());
        if (!release) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "No validated Agent release is published"});
        }
        stage = "control_dispatch";
        const auto dispatched = co_await dispatchDeviceUpdate(context.worker(), runtime, device, user, *release);
        if (dispatched.status == DeviceUpdateDispatch::kOffline) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Agent is offline or requires an initial installer upgrade"});
        }
        if (dispatched.status == DeviceUpdateDispatch::kCurrent) {
            DeviceUpdateResponse response;
            response.set<"status">("current").set<"version">(dispatched.version);
            co_return jsonResponse(context, response);
        }
        if (dispatched.status == DeviceUpdateDispatch::kBusy) {
            co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_busy", .message_ = "Agent control queue is unavailable"});
        }
        DeviceUpdateResponse response;
        response.set<"status">("requested").set<"version">(dispatched.version);
        co_return jsonResponse(context, response, {.status_ = ruvia::http_status::accepted});
    } catch (const std::exception& error) {
        // Fixed phase and exception type identify failures without exposing
        // request bodies, credentials, database errors or exception messages.
        spdlog::error("agent_update_failed stage={} exception_type={}", stage, typeid(error).name());
        throw;
    }
}
ruvia::task<ruvia::http_response> DeviceInstallerController::script(ruvia::context& context) {
    context.policy().header("Cache-Control", "no-store");
    co_return context.text(kWindowsInstallScript);
}
ruvia::task<ruvia::http_response> DeviceController::install(ruvia::context& context) {
    context.policy().header("Cache-Control", "no-store");
    context.policy().header("Referrer-Policy", "no-referrer");
    const auto code = std::string(context.request().query("code").value_or(""));
    const auto repair = std::string(context.request().query("device").value_or(""));
    std::optional<std::string> device;
    if (!code.empty() && repair.empty()) {
        device = co_await context.worker_state<ServerRuntime>().devices.enrollmentDevice(context.db(), code);
    } else if (code.empty() && repair.starts_with("d-") && validToken(std::string_view(repair).substr(2))) {
        // Repair carries no enrollment capability; the installer requires this
        // exact identity and its existing DPAPI credential on the local machine.
        device = repair;
    }
    if (!device) {
        co_return context.text(ruvia::static_text("throw 'Installation command expired or invalid. Copy a new command from device management.'\n"), {.status_ = ruvia::http_status::not_found});
    }
    const auto& origin = context.worker_state<ServerRuntime>().config.origin;
    auto script = "& { $utOrigin='" + origin + "'; $utDevice='" + *device + "'; $utCode='" + code + "'; $ErrorActionPreference='Stop'; irm ($utOrigin + '/install.ps1') | iex }\n";
    co_return context.text(std::move(script));
}
ruvia::task<ruvia::http_response> DeviceController::installManifest(ruvia::context& context, const InstallManifestRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    const auto code = optionalTextValue(body.get<"code">());
    const auto credential = optionalTextValue(body.get<"credential">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool authorized = co_await authorizeInstallerRequest(runtime.devices, context.db(), device, code, credential);
    if (!authorized) {
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "unauthorized", .message_ = "Valid device enrollment code or credential is required"});
    }
    const auto release = ((co_await context.run_blocking([root = runtime.config.agentReleaseDir] { return agentRelease(root); })).take_value());
    if (!release) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "No validated Agent release is published"});
    }
    const auto& config = runtime.config;
    InstallManifestResponse response;
    response.set<"version">(release->update.version())
        .set<"file">(release->file.filename().string())
        .set<"sha256">(release->update.sha256())
        .set<"size">(release->update.size())
        .set<"native_host">(config.nativeHost)
        .set<"native_address">(config.nativePublicAddress)
        .set<"native_port">(config.nativePort);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::installPackage(ruvia::context& context, const InstallPackageRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    const auto code = optionalTextValue(body.get<"code">());
    const auto credential = optionalTextValue(body.get<"credential">());
    const auto expectedSha256 = optionalTextValue(body.get<"sha256">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool authorized = co_await authorizeInstallerRequest(runtime.devices, context.db(), device, code, credential);
    if (!authorized) {
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "unauthorized", .message_ = "Valid device enrollment code or credential is required"});
    }
    const auto release = ((co_await context.run_blocking([root = runtime.config.agentReleaseDir] { return agentRelease(root); })).take_value());
    if (!release || (!expectedSha256.empty() && release->update.sha256() != expectedSha256)) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "Requested Agent package is unavailable"});
    }
    if (release->update.size() == 0 || release->update.size() > 64ull * 1024 * 1024) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "Agent package size exceeds limit"});
    }
    context.policy().header("Cache-Control", "no-store");
    co_return co_await context.file({.path_ = release->file, .content_type_ = "application/octet-stream"});
}
ruvia::task<ruvia::http_response> DeviceController::rename(ruvia::context& context, const DeviceRenameRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    const auto name = textValue(body.get<"name">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool updated = co_await runtime.devices.rename(context.db(), context.request_state<UserIdentity>().id, device, name);
    if (!updated) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can rename an active device"});
    }
    publishClusterStatus(runtime, "d:" + device);
    DeviceRenameResponse response;
    response.set<"id">(device).set<"name">(name);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::transferOwner(ruvia::context& context, const DeviceTransferOwnerRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    const auto owner = textValue(body.get<"owner_id">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const auto db = context.db();
    const bool transferred = user.role == "admin" && (co_await runtime.identities.reauthenticate(db, user, optionalTextValue(body.get<"current_password">()), optionalTextValue(body.get<"code">()), optionalTextValue(body.get<"telegram_proof">()))) && (co_await runtime.devices.transferOwner(db, user.id, device, owner));
    if (!transferred) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "owner_transfer_rejected", .message_ = "Owner transfer rejected"});
    }
    co_await revalidateAccess(context);
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    DeviceTransferOwnerResponse response;
    response.set<"id">(device).set<"owner_id">(owner);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::remove(ruvia::context& context, const DeviceIdRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    const bool removed = co_await runtime.devices.remove(db, context.request_state<UserIdentity>().id, device);
    if (!removed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can delete a device"});
    }
    closeDeviceControl(runtime, device);
    const auto sessions = co_await runtime.sessions.revokeDevice(db, device);
    for (const auto& id : sessions) {
        closeDesktopSession(runtime, id, DesktopCloseReason::kDeviceDeleted);
    }
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    DeviceDeleteResponse response;
    response.set<"id">(device).set<"deleted">(true);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::windowsLogin(ruvia::context& context, const WindowsLoginApiRequest& body) {
    const auto device = textValue(body.get<"device_id">());
    const auto action = textValue(body.get<"action">());
    wire::WindowsLoginRequest request;
    request.set_action(action == "read" ? wire::WindowsLoginRequest::READ : action == "save" ? wire::WindowsLoginRequest::SAVE
                                                                        : action == "clear"  ? wire::WindowsLoginRequest::CLEAR
                                                                                             : wire::WindowsLoginRequest::UNSPECIFIED);
    if (request.action() == wire::WindowsLoginRequest::SAVE) {
        if (const auto username = optionalTextValue(body.get<"username">()); !username.empty()) {
            request.set_username(username);
        }
        if (const auto password = optionalTextValue(body.get<"password">()); !password.empty()) {
            request.set_password(password);
        }
        if (const auto domain = optionalTextValue(body.get<"domain">()); !domain.empty()) {
            request.set_domain(domain);
        }
    } else if (body.get<"username">().has_value() || body.get<"domain">().has_value() || body.get<"password">().has_value() || request.action() == wire::WindowsLoginRequest::UNSPECIFIED) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Invalid Windows credential operation"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    const bool allowed = co_await runtime.devices.canConfigureWindowsLogin(db, context.request_state<UserIdentity>().id, device);
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Windows credential configuration requires control permission"});
    }
    const auto online = co_await queryDeviceOnlineState(context.worker(), runtime, device);
    if (!online.online) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Agent is offline or does not support Windows credentials"});
    }
    auto result = co_await requestDeviceWindowsLogin(context.worker(), runtime, device, std::move(request));
    co_await runtime.devices.auditWindowsLogin(db, context.request_state<UserIdentity>().id, device, action, result && result->success());
    if (!result || !result->success()) {
        co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_settings_failed", .message_ = "Agent could not complete the credential operation; read the saved state before retrying"});
    }
    WindowsLoginResponse response;
    response.set<"saved">(result->saved()).set<"username">(result->username()).set<"domain">(result->domain());
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::restartPermissionStatus(ruvia::context& context, const RestartPermissionQuery& query) {
    const auto device = textValue(query.get<"deviceId">());
    const auto allowed = co_await context.worker_state<ServerRuntime>().devices.systemRestartPermission(context.db(), context.request_state<UserIdentity>().id, device);
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can read restart permission"});
    }
    RestartPermissionResponse response;
    response.set<"allowed">(*allowed);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::restart(ruvia::context& context, const DeviceRestartRequest& body) {
    const auto device = textValue(body.get<"deviceId">());
    const auto key = textValue(body.get<"idempotencyKey">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>().id;
    const auto db = context.db();
    auto operation = co_await runtime.devices.restartByIdempotencyKey(db, user, device, key);
    bool dispatchNew = false;
    if (!operation) {
        const auto permission = co_await runtime.devices.systemRestartPermission(db, user, device);
        if (!permission || !*permission) {
            co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Restart is not permitted"});
        }
        const auto capability = co_await queryDeviceRestartCapability(context.worker(), runtime, device);
        if (!capability.ready) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Device is offline or does not support restart"});
        }
        operation = co_await runtime.devices.createRestart(db, user, device, key, capability.bootTimeUnixSeconds);
        dispatchNew = true;
    }
    if (!operation) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Restart is not permitted"});
    }
    if (operation->newlyCreated && dispatchNew) {
        wire::WindowsRestartRequest request;
        request.set_operation_id(operation->id);
        request.set_device_id(device);
        request.set_expires_at_unix_seconds(operation->expiresAtUnixSeconds);
        // This handoff is the ownership boundary: HTTP never waits for the Agent.
        // Cancellation before dispatch leaves the durable PENDING record to expire.
        const bool enqueued = co_await enqueueDeviceRestart(context.worker(), runtime, device, std::move(request));
        if (!enqueued) {
            co_await runtime.devices.updateRestart(db, operation->id, "FAILED");
            operation->status = "FAILED";
        }
    }
    const auto state = operation->status == "RESOLVED" ? "resolved" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                  : operation->status == "UNKNOWN"    ? "unknown"
                                                                  : operation->status == "FAILED"     ? "failed"
                                                                  : operation->status == "COMPLETED"  ? "confirmed"
                                                                                                      : "requested";
    RestartOperationResponse response;
    response.set<"id">(operation->id).set<"status">(state);
    co_return jsonResponse(context, response, {.status_ = ruvia::http_status::accepted});
}
ruvia::task<ruvia::http_response> DeviceController::restartStatus(ruvia::context& context) {
    const auto id = std::string(context.request().param("id").value_or(""));
    const auto operation = co_await context.worker_state<ServerRuntime>().devices.restart(context.db(), context.request_state<UserIdentity>().id, id);
    if (!operation) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Restart operation not found"});
    }
    const auto status = operation->status == "PENDING" ? "requested" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                   : operation->status == "COMPLETED"  ? "confirmed"
                                                                   : operation->status == "FAILED"     ? "failed"
                                                                   : operation->status == "RESOLVED"   ? "resolved"
                                                                                                       : "unknown";
    RestartOperationResponse response;
    response.set<"id">(operation->id).set<"status">(status);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::enrollment(ruvia::context& context, const DeviceEnrollmentRequest& body) {
    const auto id = textValue(body.get<"device_id">());
    if (body.get<"platform">().view() != "windows-x64") {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "unsupported_platform", .message_ = "Platform not supported"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto code = co_await runtime.devices.createEnrollment(context.db(), context.request_state<UserIdentity>().id, id);
    if (!code) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device owner can install an Agent"});
    }
    const auto& config = runtime.config;
    DeviceEnrollmentResponse response;
    response.set<"code">(*code)
        .set<"device_id">(id)
        .set<"expires_in">(code->empty() ? 0 : 600)
        .set<"web_origin">(config.origin)
        .set<"native_host">(config.nativeHost)
        .set<"native_address">(config.nativePublicAddress)
        .set<"native_port">(config.nativePort);
    co_return jsonResponse(context, response, {.status_ = ruvia::http_status::created});
}
ruvia::task<ruvia::http_response> DeviceController::create(ruvia::context& context, const DeviceCreateRequest& body) {
    const auto name = textValue(body.get<"name">());
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto device = co_await runtime.devices.createDevice(context.db(), context.request_state<UserIdentity>().id, name);
    publishClusterStatus(runtime, "d:" + device.id);
    DeviceCreateResponse response;
    response.set<"id">(device.id)
        .set<"name">(device.name)
        .set<"enrolled">(false)
        .set<"can_manage">(true)
        .set<"desktop">(true)
        .set<"write_control">(true)
        .set<"verification_method">("direct")
        .set<"manage_credentials">(true)
        .set<"online">(false)
        .set_null<"last_seen">();
    co_return jsonResponse(context, response, {.status_ = ruvia::http_status::created});
}
ruvia::task<ruvia::http_response> DeviceController::devices(ruvia::context& context) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto devices = co_await runtime.devices.list(context.db(), context.request_state<UserIdentity>().id);
    std::vector<std::string> deviceIds;
    deviceIds.reserve(devices.size());
    for (const auto& device : devices) {
        deviceIds.push_back(device.id);
    }
    const auto onlineStates = co_await queryDeviceOnlineStates(context.worker(), runtime, deviceIds);
    ruvia::array<DeviceListItem> response;
    for (const auto& device : devices) {
        const auto found = onlineStates.find(device.id);
        const auto& state = found != onlineStates.end() ? found->second : DeviceOnlineState{};
        DeviceListItem item;
        if (state.online && !state.agentVersion.empty()) {
            item.set<"agent_version">(state.agentVersion);
        } else if (device.agentVersion && !device.agentVersion->empty()) {
            item.set<"agent_version">(*device.agentVersion);
        } else {
            item.set_null<"agent_version">();
        }
        item.set<"id">(device.id)
            .set<"name">(device.name)
            .set<"enrolled">(device.enrolled)
            .set<"can_manage">(device.canManage)
            .set<"desktop">(device.desktop)
            .set<"write_control">(device.writeControl)
            .set<"verification_method">(device.verificationMethod)
            .set<"manage_credentials">(device.manageCredentials)
            .set<"online">(state.online);
        if (device.lastSeen) {
            item.set<"last_seen">(*device.lastSeen);
        } else {
            item.set_null<"last_seen">();
        }
        response.push_back(std::move(item));
    }
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::grants(ruvia::context& context) {
    const auto result = co_await context.worker_state<ServerRuntime>().devices.grants(context.db(), context.request_state<UserIdentity>().id, std::string(context.request().param("id").value_or("")));
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Device owner or administrator required"});
    }
    ruvia::array<DeviceGrantItem> grants;
    for (const auto& g : *result) {
        DeviceGrantItem item;
        item.set<"user_id">(g.userId).set<"username">(g.username).set<"connect">(g.desktop && g.writeControl).set<"verification_method">(g.verificationMethod);
        grants.push_back(std::move(item));
    }
    co_return jsonResponse(context, grants);
}
ruvia::task<ruvia::http_response> DeviceController::saveGrant(ruvia::context& context, const DeviceGrantRequest& body) {
    const auto device = std::string(context.request().param("id").value_or(""));
    const bool connect = body.get<"connect">();
    const DeviceGrant grant{{}, textValue(body.get<"username">()), connect, connect, false, false, textValue(body.get<"verification_method">())};
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool ok = co_await runtime.devices.setGrant(context.db(), context.request_state<UserIdentity>().id, device, grant, body.get<"remove">());
    if (!ok) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "grant_rejected", .message_ = "Grant rejected; check administrator or owner, target user and Telegram configuration"});
    }
    co_await revalidateAccess(context);
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, okResponse());
}
} // namespace ultraterimal::server
