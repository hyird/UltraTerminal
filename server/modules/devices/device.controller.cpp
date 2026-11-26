#include "device.controller.h"

#include <exception>
#include <fstream>
#include <typeinfo>
#include <utility>

#include <spdlog/spdlog.h>

#include "config/request.schema.h"
#include "features/desktop/access.runtime.h"
#include "installer.script.h"
#include "middleware/json.transport.h"
#include "update.service.h"
namespace ultraterimal::server {
ruvia::task<ruvia::http_response> DeviceController::update(ruvia::context& context) {
    std::string_view stage = "request_body";
    try {
        const auto body = requestObject(co_await context.request().body_text(), {"device_id"});
        const auto device = requestText(body, "device_id", 128);
        auto& runtime = context.worker_state<ServerRuntime>();
        const auto user = context.request_state<UserIdentity>().id;
        stage = "ownership_query";
        const auto allowed = ((co_await context.run_blocking([service = runtime.devices, device, user] {
                                  const auto devices = service.list(user);
                                  return std::any_of(devices.begin(), devices.end(), [&](const auto& found) { return found.id == device && found.canManage; });
                              })).take_value());
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
            co_return jsonResponse(context, {{"status", "current"}, {"version", dispatched.version}});
        }
        if (dispatched.status == DeviceUpdateDispatch::kBusy) {
            co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_busy", .message_ = "Agent control queue is unavailable"});
        }
        co_return jsonResponse(context, {{"status", "requested"}, {"version", dispatched.version}}, {.status_ = ruvia::http_status::accepted});
    } catch (const std::exception& error) {
        // Fixed phase and exception type identify failures without exposing
        // request bodies, credentials, database errors or exception messages.
        spdlog::error("agent_update_failed stage={} exception_type={}", stage, typeid(error).name());
        throw;
    }
}
ruvia::task<ruvia::http_response> DeviceInstallerController::script(ruvia::context& context) {
    context.policy().header("Cache-Control", "no-store");
    context.policy().header("X-Content-Type-Options", "nosniff");
    co_return context.text(kWindowsInstallScript);
}
ruvia::task<ruvia::http_response> DeviceController::install(ruvia::context& context) {
    context.policy().header("Cache-Control", "no-store");
    context.policy().header("Referrer-Policy", "no-referrer");
    context.policy().header("X-Content-Type-Options", "nosniff");
    const auto code = std::string(context.request().query("code").value_or(""));
    const auto repair = std::string(context.request().query("device").value_or(""));
    std::optional<std::string> device;
    if (!code.empty() && repair.empty()) {
        device = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().devices, code] { return service.enrollmentDevice(code); })).take_value());
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
namespace {
bool authorizeInstallerRequest(const DeviceService& service, const std::string& deviceId, const std::string& code, const std::string& credential) {
    if (!code.empty() && credential.empty()) {
        const auto enrolled = service.enrollmentDevice(code);
        return enrolled && *enrolled == deviceId;
    }
    if (code.empty() && !credential.empty()) {
        const auto identity = service.authenticate(credential);
        return identity && identity->id == deviceId;
    }
    return false;
}
} // namespace
ruvia::task<ruvia::http_response> DeviceController::installManifest(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "code", "credential"});
    const auto device = requestText(body, "device_id", 128);
    const auto code = optionalText(body, "code", 128);
    const auto credential = optionalText(body, "credential", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool authorized = ((co_await context.run_blocking([service = runtime.devices, device, code, credential] {
                                 return authorizeInstallerRequest(service, device, code, credential);
                             })).take_value());
    if (!authorized) {
        co_return context.error({.status_ = ruvia::http_status::unauthorized, .code_ = "unauthorized", .message_ = "Valid device enrollment code or credential is required"});
    }
    const auto release = ((co_await context.run_blocking([root = runtime.config.agentReleaseDir] { return agentRelease(root); })).take_value());
    if (!release) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "release_unavailable", .message_ = "No validated Agent release is published"});
    }
    const auto& config = runtime.config;
    co_return jsonResponse(context, {
                                        {"version", release->update.version()},
                                        {"file", release->file.filename().string()},
                                        {"sha256", release->update.sha256()},
                                        {"size", release->update.size()},
                                        {"native_host", config.nativeHost},
                                        {"native_address", config.nativePublicAddress},
                                        {"native_port", config.nativePort},
                                    });
}
ruvia::task<ruvia::http_response> DeviceController::installPackage(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "code", "credential", "sha256"});
    const auto device = requestText(body, "device_id", 128);
    const auto code = optionalText(body, "code", 128);
    const auto credential = optionalText(body, "credential", 128);
    const auto expectedSha256 = optionalText(body, "sha256", 64);
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool authorized = ((co_await context.run_blocking([service = runtime.devices, device, code, credential] {
                                 return authorizeInstallerRequest(service, device, code, credential);
                             })).take_value());
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
    auto bytes = ((co_await context.run_blocking([path = release->file, size = release->update.size()] -> std::optional<std::string> {
                      std::ifstream input(path, std::ios::binary);
                      std::string data(static_cast<std::size_t>(size), '\0');
                      input.read(data.data(), static_cast<std::streamsize>(data.size()));
                      if (!input || static_cast<std::uint64_t>(input.gcount()) != size) {
                          return std::nullopt;
                      }
                      return data;
                  })).take_value());
    if (!bytes) {
        co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "release_unavailable", .message_ = "Cannot read Agent package"});
    }
    context.policy().header("Content-Type", "application/octet-stream");
    context.policy().header("Cache-Control", "no-store");
    context.policy().header("X-Content-Type-Options", "nosniff");
    co_return context.body(std::move(*bytes));
}
ruvia::task<ruvia::http_response> DeviceController::rename(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "name"});
    const auto device = requestText(body, "device_id", 128);
    const auto name = requestText(body, "name", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool updated = ((co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device, name] { return service.rename(user, device, name); })).take_value());
    if (!updated) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can rename an active device"});
    }
    publishClusterStatus(runtime, "d:" + device);
    co_return jsonResponse(context, {{"id", device}, {"name", name}});
}
ruvia::task<ruvia::http_response> DeviceController::transferOwner(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "owner_id", "current_password", "code", "telegram_proof"});
    const auto device = requestText(body, "device_id", 128);
    const auto owner = requestText(body, "owner_id", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>();
    const bool transferred = ((co_await context.run_blocking([identities = runtime.identities, devices = runtime.devices, user, device, owner, password = optionalText(body, "current_password", 1024), code = optionalText(body, "code", 32), telegramProof = optionalText(body, "telegram_proof", 32)] {
                                  return user.role == "admin" && identities.reauthenticate(user, password, code, telegramProof) && devices.transferOwner(user.id, device, owner);
                              })).take_value());
    if (!transferred) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "owner_transfer_rejected", .message_ = "Owner transfer rejected"});
    }
    co_await revalidateAccess(context);
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, {{"id", device}, {"owner_id", owner}});
}
ruvia::task<ruvia::http_response> DeviceController::remove(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id"});
    const auto device = requestText(body, "device_id", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool removed = ((co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device] { return service.remove(user, device); })).take_value());
    if (!removed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can delete a device"});
    }
    closeDeviceControl(runtime, device);
    const auto sessions = ((co_await context.run_blocking([service = runtime.sessions, device] { return service.revokeDevice(device); })).take_value());
    for (const auto& id : sessions) {
        closeDesktopSession(runtime, id, DesktopCloseReason::kDeviceDeleted);
    }
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, {{"id", device}, {"deleted", true}});
}
ruvia::task<ruvia::http_response> DeviceController::windowsLogin(ruvia::context& context) {
    auto body = requestObject(co_await context.request().body_text(), {"device_id", "action", "username", "domain", "password"});
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
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool allowed = ((co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device] { return service.canConfigureWindowsLogin(user, device); })).take_value());
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Windows credential configuration requires control permission"});
    }
    const auto online = co_await queryDeviceOnlineState(context.worker(), runtime, device);
    if (!online.online) {
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Agent is offline or does not support Windows credentials"});
    }
    auto result = co_await requestDeviceWindowsLogin(context.worker(), runtime, device, std::move(request));
    ((co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device, action, success = result && result->success()] { service.auditWindowsLogin(user, device, action, success); })).take_value());
    if (!result || !result->success()) {
        co_return context.error({.status_ = ruvia::http_status::service_unavailable, .code_ = "agent_settings_failed", .message_ = "Agent could not complete the credential operation; read the saved state before retrying"});
    }
    co_return jsonResponse(context, {{"saved", result->saved()}, {"username", result->username()}, {"domain", result->domain()}});
}
ruvia::task<ruvia::http_response> DeviceController::restartPermissionStatus(ruvia::context& context) {
    const auto device = std::string(context.request().query("deviceId").value_or(""));
    if (device.empty() || device.size() > 128) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "deviceId is required"});
    }
    const auto allowed = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().devices, user = context.request_state<UserIdentity>().id, device] { return service.systemRestartPermission(user, device); })).take_value());
    if (!allowed) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device registrant can read restart permission"});
    }
    co_return jsonResponse(context, {{"allowed", *allowed}});
}
ruvia::task<ruvia::http_response> DeviceController::restart(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"deviceId", "idempotencyKey"});
    const auto device = requestText(body, "deviceId", 128);
    const auto key = requestText(body, "idempotencyKey", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto user = context.request_state<UserIdentity>().id;
    auto operation = ((co_await context.run_blocking([service = runtime.devices, user, device, key] { return service.restartByIdempotencyKey(user, device, key); })).take_value());
    bool dispatchNew = false;
    if (!operation) {
        const bool allowed = ((co_await context.run_blocking([service = runtime.devices, user, device] { const auto permission = service.systemRestartPermission(user, device); return permission && *permission; })).take_value());
        if (!allowed) {
            co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Restart is not permitted"});
        }
        const auto capability = co_await queryDeviceRestartCapability(context.worker(), runtime, device);
        if (!capability.ready) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "agent_unavailable", .message_ = "Device is offline or does not support restart"});
        }
        operation = ((co_await context.run_blocking([service = runtime.devices, user, device, key, boot = capability.bootTimeUnixSeconds] { return service.createRestart(user, device, key, boot); })).take_value());
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
            ((co_await context.run_blocking([service = runtime.devices, id = operation->id] { service.updateRestart(id, "FAILED"); })).take_value());
            operation->status = "FAILED";
        }
    }
    const auto state = operation->status == "RESOLVED" ? "resolved" : operation->status == "ACCEPTED" ? "agent_accepted"
                                                                  : operation->status == "UNKNOWN"    ? "unknown"
                                                                  : operation->status == "FAILED"     ? "failed"
                                                                  : operation->status == "COMPLETED"  ? "confirmed"
                                                                                                      : "requested";
    co_return jsonResponse(context, {{"id", operation->id}, {"status", state}}, {.status_ = ruvia::http_status::accepted});
}
ruvia::task<ruvia::http_response> DeviceController::restartStatus(ruvia::context& context) {
    const auto id = std::string(context.request().param("id").value_or(""));
    const auto operation = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().devices, user = context.request_state<UserIdentity>().id, id] { return service.restart(user, id); })).take_value());
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
ruvia::task<ruvia::http_response> DeviceController::enrollment(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"device_id", "platform"});
    const auto id = requestText(body, "device_id", 128);
    if (requestText(body, "platform", 32) != "windows-x64") {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "unsupported_platform", .message_ = "Platform not supported"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto code = ((co_await context.run_blocking([service = runtime.devices, userId = context.request_state<UserIdentity>().id, id] { return service.createEnrollment(userId, id); })).take_value());
    if (!code) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Only the device owner can install an Agent"});
    }
    const auto& config = runtime.config;
    co_return jsonResponse(context, {{"code", *code}, {"device_id", id}, {"expires_in", code->empty() ? 0 : 600}, {"web_origin", config.origin}, {"native_host", config.nativeHost}, {"native_address", config.nativePublicAddress}, {"native_port", config.nativePort}}, {.status_ = ruvia::http_status::created});
}
ruvia::task<ruvia::http_response> DeviceController::create(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"name"});
    const auto name = requestText(body, "name", 128);
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto device = ((co_await context.run_blocking([service = runtime.devices, userId = context.request_state<UserIdentity>().id, name] { return service.createDevice(userId, name); })).take_value());
    publishClusterStatus(runtime, "d:" + device.id);
    co_return jsonResponse(context, {{"id", device.id}, {"name", device.name}, {"enrolled", false}, {"can_manage", true}, {"desktop", true}, {"write_control", true}, {"verification_method", "direct"}, {"manage_credentials", true}, {"online", false}, {"last_seen", nullptr}}, {.status_ = ruvia::http_status::created});
}
ruvia::task<ruvia::http_response> DeviceController::devices(ruvia::context& context) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto devices = ((co_await context.run_blocking([service = runtime.devices, userId = context.request_state<UserIdentity>().id] { return service.list(userId); })).take_value());
    std::vector<std::string> deviceIds;
    deviceIds.reserve(devices.size());
    for (const auto& device : devices) {
        deviceIds.push_back(device.id);
    }
    const auto onlineStates = co_await queryDeviceOnlineStates(context.worker(), runtime, deviceIds);
    boost::json::array response;
    for (const auto& device : devices) {
        const auto found = onlineStates.find(device.id);
        const auto& state = found != onlineStates.end() ? found->second : DeviceOnlineState{};
        const auto agentVersion = state.online && !state.agentVersion.empty()            ? boost::json::value(state.agentVersion)
                                  : device.agentVersion && !device.agentVersion->empty() ? boost::json::value(*device.agentVersion)
                                                                                         : boost::json::value(nullptr);
        boost::json::object item{{"agent_version", agentVersion}, {"id", device.id}, {"name", device.name}, {"enrolled", device.enrolled}, {"can_manage", device.canManage}, {"desktop", device.desktop}, {"write_control", device.writeControl}, {"verification_method", device.verificationMethod}, {"manage_credentials", device.manageCredentials}, {"online", state.online}};
        item["last_seen"] = device.lastSeen ? boost::json::value(*device.lastSeen) : boost::json::value(nullptr);
        response.push_back(std::move(item));
    }
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> DeviceController::grants(ruvia::context& context) {
    const auto result = ((co_await context.run_blocking([service = context.worker_state<ServerRuntime>().devices, user = context.request_state<UserIdentity>().id, device = std::string(context.request().param("id").value_or(""))] { return service.grants(user, device); })).take_value());
    if (!result) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "access_denied", .message_ = "Device owner or administrator required"});
    }
    boost::json::array grants;
    for (const auto& g : *result) {
        grants.push_back(boost::json::object{{"user_id", g.userId}, {"username", g.username}, {"connect", g.desktop && g.writeControl}, {"verification_method", g.verificationMethod}});
    }
    co_return jsonResponse(context, grants);
}
ruvia::task<ruvia::http_response> DeviceController::saveGrant(ruvia::context& context) {
    const auto body = requestObject(co_await context.request().body_text(), {"username", "connect", "verification_method", "remove"});
    const auto device = std::string(context.request().param("id").value_or(""));
    const DeviceGrant grant{{}, requestText(body, "username", 64), requestBool(body, "connect"), requestBool(body, "connect"), false, false, requestText(body, "verification_method", 16)};
    auto& runtime = context.worker_state<ServerRuntime>();
    const bool ok = ((co_await context.run_blocking([service = runtime.devices, user = context.request_state<UserIdentity>().id, device, grant, remove = requestBool(body, "remove")] { return service.setGrant(user, device, grant, remove); })).take_value());
    if (!ok) {
        co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = "grant_rejected", .message_ = "Grant rejected; check administrator or owner, target user and Telegram configuration"});
    }
    co_await revalidateAccess(context);
    publishClusterStatus(runtime, "d:" + device);
    publishClusterStatus(runtime, "u:*");
    co_return jsonResponse(context, {{"ok", true}});
}
} // namespace ultraterimal::server
