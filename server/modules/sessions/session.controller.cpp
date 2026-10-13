#include "session.controller.h"

#include "config/request.schema.h"
#include "middleware/json.transport.h"

namespace ultraterimal::server {
namespace {
bool boolean(const boost::json::object& object, std::string_view name) {
    const auto* field = object.if_contains(name);
    if (!field || !field->is_bool()) {
        throw ruvia::http_error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Expected a boolean"});
    }
    return field->as_bool();
}
} // namespace
ruvia::task<ruvia::http_response> SessionController::create(ruvia::context& context) {
    const auto body = requestObject(co_await context.req().text(), {"device_id", "kind", "write_control", "preempt", "desktop_input_contract", "approval_id", "verification_code", "reconnect_session_id"});
    SessionRequest request;
    request.verificationCode = optionalText(body, "verification_code", 6);
    request.reconnectSessionId = optionalText(body, "reconnect_session_id", 128);
    request.userId = context.request_state<UserIdentity>().id;
    request.loginHash = context.request_state<UserIdentity>().loginHash;
    if (body.contains("approval_id")) {
        request.approvalId = requestText(body, "approval_id", 32);
    }
    request.deviceId = requestText(body, "device_id", 128);
    const auto kind = requestText(body, "kind", 16);
    request.kind = kind == "desktop" ? wire::DESKTOP : kind == "vnc" ? wire::VNC
                                                   : kind == "ssh"   ? wire::SSH
                                                                     : wire::SESSION_UNSPECIFIED;
    request.writeControl = boolean(body, "write_control");
    const auto* preempt = body.if_contains("preempt");
    if (preempt && !preempt->is_bool()) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Expected a boolean"});
    }
    request.preempt = preempt && preempt->as_bool();
    if (request.kind == wire::SESSION_UNSPECIFIED) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_kind", .message_ = "Unsupported session kind"});
    }
    const auto* contract = body.if_contains("desktop_input_contract");
    if (request.kind == wire::DESKTOP && request.writeControl) {
        if (!contract || !contract->is_int64() || contract->as_int64() != 2) {
            co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "desktop_input_upgrade_required", .message_ = "Writable desktop requires the input generation contract"});
        }
        request.desktopInputContract = 2;
    } else if (contract) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Input contract only applies to writable desktop sessions"});
    }
    auto& runtime = (*context.worker_state<std::shared_ptr<ServerRuntime>>());
    const auto found = runtime.controlConnections.find(request.deviceId);
    std::shared_ptr<ControlConnection> control = found == runtime.controlConnections.end() ? nullptr : found->second;
    request.agentInputContractSupported = control && control->ready && control->capabilities.capture_available() && control->capabilities.desktop_input_contract() == 2;
    // Authorize before exposing online/capability state or dispatching to the device.
    const auto issued = (co_await context.run_blocking([service = runtime.sessions, request] { return service.create(request); }));
    if (!issued) {
        if (issued.error() == SessionFailure::kVerificationRequired || issued.error() == SessionFailure::kVerificationFailed) {
            co_return context.error({.status_ = ruvia::http_status::forbidden, .code_ = issued.error() == SessionFailure::kVerificationRequired ? "connection_verification_required" : "connection_verification_failed", .message_ = "Authenticator verification required"});
        }
        if (issued.error() == SessionFailure::kApprovalRequired) {
            co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "approval_required", .message_ = "Requester Telegram verification required"});
        }
        const auto status = issued.error() == SessionFailure::kDenied ? ruvia::http_status::forbidden : issued.error() == SessionFailure::kInvalid ? ruvia::http_status::bad_request
                                                                                                                                                   : ruvia::http_status::conflict;
        co_return context.error({.status_ = status, .code_ = issued.error() == SessionFailure::kIncompatible ? "desktop_input_upgrade_required" : "session_rejected", .message_ = "Session request rejected"});
    }
    for (const auto& sessionId : issued->preemptedSessions) {
        const auto active = runtime.desktopSessions.find(sessionId);
        if (active != runtime.desktopSessions.end()) {
            active->second->close(DesktopCloseReason::kPreempted);
        }
        if (control) {
            wire::Envelope close;
            close.set_session_id(sessionId);
            close.mutable_session_close()->set_reason("preempted");
            if (!control->enqueue(std::move(close))) {
                control->close();
                control.reset();
            }
        }
    }
    bool capable = false;
    if (control && control->ready) {
        const auto& caps = control->capabilities;
        capable = request.kind == wire::DESKTOP ? caps.capture_available() && (!request.writeControl || caps.desktop_input_contract() == request.desktopInputContract) : request.kind == wire::VNC ? caps.vnc()
                                                                                                                                                                                                   : caps.ssh() && caps.conpty();
    }
    wire::Envelope command;
    command.set_session_id(issued->id);
    auto* open = command.mutable_session_open();
    open->set_kind(request.kind);
    open->set_user_id(request.userId);
    open->set_write_control(request.writeControl);
    open->set_desktop_input_contract(request.desktopInputContract);
    boost::json::array channels;
    for (const auto& channel : issued->channels) {
        auto* grant = open->add_channels();
        grant->set_channel_id(channel.id);
        grant->set_purpose(channel.purpose);
        grant->set_ticket(channel.agentTicket);
        channels.push_back(boost::json::object{{"id", channel.id}, {"purpose", static_cast<int>(channel.purpose)}, {"ticket", channel.browserTicket}});
    }
    if (!capable || !control->enqueue(std::move(command))) {
        (co_await context.run_blocking([service = runtime.sessions, user = request.userId, session = issued->id] { service.close(user, session, SessionEnd::kFailure); }));
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "Device capability or control connection unavailable"});
    }
    context.status(ruvia::http_status::accepted);
    co_return jsonResponse(context, {{"id", issued->id}, {"state", "pending"}, {"expires_in", 60}, {"channels", std::move(channels)}});
}
ruvia::task<ruvia::http_response> SessionController::get(ruvia::context& context) {
    const auto info = (co_await context.run_blocking([service = (*context.worker_state<std::shared_ptr<ServerRuntime>>()).sessions, user = context.request_state<UserIdentity>().id, session = std::string(context.req().param("id").value_or(""))] { return service.get(user, session); }));
    if (!info) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Session not found"});
    }
    co_return jsonResponse(context, {{"id", info->id}, {"device_id", info->deviceId}, {"state", info->state}, {"end_reason", info->endReason}});
}
ruvia::task<ruvia::http_response> SessionController::close(ruvia::context& context) {
    (void)requestObject(co_await context.req().text(), {});
    auto& runtime = (*context.worker_state<std::shared_ptr<ServerRuntime>>());
    const auto info = (co_await context.run_blocking([service = runtime.sessions, user = context.request_state<UserIdentity>().id, session = std::string(context.req().param("id").value_or(""))] {
        const auto info = service.get(user, session);
        if (info) {
            service.close(user, session, SessionEnd::kUser);
        }
        return info;
    }));
    if (!info) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Session not found"});
    }
    if (info->state != "closed") {
        const auto active = runtime.desktopSessions.find(info->id);
        if (active != runtime.desktopSessions.end()) {
            active->second->close(DesktopCloseReason::kUserClosed);
        }
        const auto found = runtime.controlConnections.find(info->deviceId);
        if (found != runtime.controlConnections.end()) {
            wire::Envelope command;
            command.set_session_id(info->id);
            command.mutable_session_close()->set_reason("user_closed");
            if (!found->second->enqueue(std::move(command))) {
                found->second->close();
            }
        }
    }
    co_return jsonResponse(context, {{"id", info->id}, {"state", "closed"}});
}
} // namespace ultraterimal::server
