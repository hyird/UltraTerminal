#include "session.controller.h"

namespace ultraterimal::server {
ruvia::task<ruvia::http_response> SessionController::create(ruvia::context& context, const SessionCreateRequest& body) {
    SessionRequest request;
    request.verificationCode = optionalTextValue(body.get<"verification_code">());
    request.reconnectSessionId = optionalTextValue(body.get<"reconnect_session_id">());
    request.userId = context.request_state<UserIdentity>().id;
    request.loginHash = context.request_state<UserIdentity>().loginHash;
    if (const auto& approvalId = body.get<"approval_id">()) {
        request.approvalId = textValue(*approvalId);
    }
    request.deviceId = textValue(body.get<"device_id">());
    const auto kind = body.get<"kind">().view();
    request.kind = kind == "desktop" ? wire::DESKTOP : kind == "vnc" ? wire::VNC
                                                   : kind == "ssh"   ? wire::SSH
                                                                     : wire::SESSION_UNSPECIFIED;
    request.writeControl = body.get<"write_control">();
    request.preempt = body.get<"preempt">().value_or(false);
    if (request.kind == wire::SESSION_UNSPECIFIED) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_kind", .message_ = "Unsupported session kind"});
    }
    const auto contract = body.get<"desktop_input_contract">();
    if (request.kind == wire::DESKTOP && request.writeControl) {
        if (!contract || *contract != 2) {
            co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "desktop_input_upgrade_required", .message_ = "Writable desktop requires the input generation contract"});
        }
        request.desktopInputContract = 2;
    } else if (contract) {
        co_return context.error({.status_ = ruvia::http_status::bad_request, .code_ = "invalid_field", .message_ = "Input contract only applies to writable desktop sessions"});
    }
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto caps = co_await queryDeviceSessionCapabilities(context.worker(), runtime, request.deviceId, request.kind, request.writeControl, request.desktopInputContract);
    request.agentInputContractSupported = caps.inputSupported;
    const auto db = context.db();
    // Authorize before exposing online/capability state or dispatching to the device.
    const auto issued = co_await runtime.sessions.create(db, request);
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
        closeDesktopSession(runtime, sessionId, DesktopCloseReason::kPreempted);
    }
    wire::Envelope command;
    command.set_session_id(issued->id);
    auto* open = command.mutable_session_open();
    open->set_kind(request.kind);
    open->set_user_id(request.userId);
    open->set_write_control(request.writeControl);
    open->set_desktop_input_contract(request.desktopInputContract);
    SessionCreateResponse response;
    response.set<"id">(issued->id).set<"state">("pending").set<"expires_in">(60);
    auto& channels = response.ensure<"channels">();
    for (const auto& channel : issued->channels) {
        auto* grant = open->add_channels();
        grant->set_channel_id(channel.id);
        grant->set_purpose(channel.purpose);
        grant->set_ticket(channel.agentTicket);
        SessionChannelItem item;
        item.set<"id">(channel.id).set<"purpose">(static_cast<std::int32_t>(channel.purpose)).set<"ticket">(channel.browserTicket);
        channels.push_back(std::move(item));
    }
    const bool dispatched = caps.requestedSupported && co_await dispatchSessionOpen(context.worker(), runtime, request.deviceId, std::move(command), issued->preemptedSessions);
    if (!dispatched) {
        (void)co_await runtime.sessions.close(db, request.userId, issued->id, SessionEnd::kFailure);
        co_return context.error({.status_ = ruvia::http_status::conflict, .code_ = "device_unavailable", .message_ = "Device capability or control connection unavailable"});
    }
    co_return jsonResponse(context, response, {.status_ = ruvia::http_status::accepted});
}
ruvia::task<ruvia::http_response> SessionController::get(ruvia::context& context) {
    const auto info = co_await context.worker_state<ServerRuntime>().sessions.get(context.db(), context.request_state<UserIdentity>().id, std::string(context.request().param("id").value_or("")));
    if (!info) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Session not found"});
    }
    SessionInfoResponse response;
    response.set<"id">(info->id).set<"device_id">(info->deviceId).set<"state">(info->state).set<"end_reason">(info->endReason);
    co_return jsonResponse(context, response);
}
ruvia::task<ruvia::http_response> SessionController::close(ruvia::context& context) {
    auto& runtime = context.worker_state<ServerRuntime>();
    const auto db = context.db();
    const auto user = context.request_state<UserIdentity>().id;
    const auto session = std::string(context.request().param("id").value_or(""));
    const auto info = co_await runtime.sessions.get(db, user, session);
    if (!info) {
        co_return context.error({.status_ = ruvia::http_status::not_found, .code_ = "not_found", .message_ = "Session not found"});
    }
    (void)co_await runtime.sessions.close(db, user, session, SessionEnd::kUser);
    if (info->state != "closed") {
        closeDesktopSession(runtime, info->id, DesktopCloseReason::kUserClosed);
        closeTerminalSession(runtime, info->id, TerminalCloseReason::kUserClosed);
        sendDeviceSessionClose(runtime, info->deviceId, info->id, "user_closed");
    }
    SessionCloseResponse response;
    response.set<"id">(info->id).set<"state">("closed");
    co_return jsonResponse(context, response);
}
} // namespace ultraterimal::server
