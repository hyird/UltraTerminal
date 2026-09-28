#pragma once
#include <exception>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include "schemas/session.pb.h"

namespace ultraterimal::agent {
// Exception text stays in the local event log. Only these fixed values cross
// the worker exit-status boundary and the authenticated Agent control stream.
class DesktopFailure : public std::runtime_error {
  public:
    DesktopFailure(wire::DesktopFault::Code code, const char* message, wire::DesktopFault::Stage stage = wire::DesktopFault::STAGE_UNKNOWN)
        : std::runtime_error(message), code_(code), stage_(stage) {}
    wire::DesktopFault::Code code() const noexcept { return code_; }
    wire::DesktopFault::Stage stage() const noexcept { return stage_; }

  private:
    wire::DesktopFault::Code code_;
    wire::DesktopFault::Stage stage_;
};
inline wire::DesktopFault::Stage desktopFailureStage(std::string_view stage) noexcept {
    using Fault = wire::DesktopFault;
    static constexpr std::pair<std::string_view, Fault::Stage> stages[] = {
        {"startup", Fault::WORKER_STARTUP}, {"worker", Fault::WORKER_STARTUP}, {"video_start", Fault::VIDEO_START}, {"capture_encode", Fault::CAPTURE_ENCODE}, {"display_resize", Fault::DISPLAY_RESIZE}, {"input_binding", Fault::INPUT_BINDING}, {"input_release", Fault::INPUT_RELEASE}, {"input_apply", Fault::INPUT_APPLY}, {"input_validation", Fault::INPUT_VALIDATION}, {"input_ipc_read", Fault::INPUT_IPC_READ}, {"control_ipc_write", Fault::CONTROL_IPC_WRITE}, {"video_ipc_write", Fault::VIDEO_IPC_WRITE}, {"liveness", Fault::LIVENESS}, {"native_pairing", Fault::NATIVE_PAIRING}, {"worker_launch", Fault::WORKER_LAUNCH}, {"worker_pipe_pairing", Fault::WORKER_PIPE_PAIRING}, {"login_broker", Fault::LOGIN_BROKER}, {"session_bridge", Fault::SESSION_BRIDGE}, {"bridge_worker_read", Fault::BRIDGE_WORKER_READ}, {"bridge_control_send", Fault::BRIDGE_CONTROL_SEND}, {"bridge_video_send", Fault::BRIDGE_VIDEO_SEND}, {"bridge_input_receive", Fault::BRIDGE_INPUT_RECEIVE}, {"bridge_video_receive", Fault::BRIDGE_VIDEO_RECEIVE}};
    for (const auto& [name, value] : stages) {
        if (name == stage) {
            return value;
        }
    }
    return Fault::STAGE_UNKNOWN;
}
inline wire::DesktopFault::Code desktopFailureCode(std::exception_ptr failure, wire::DesktopFault::Stage stage) noexcept {
    using Fault = wire::DesktopFault;
    try {
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const DesktopFailure& error) {
        return error.code();
    } catch (const std::system_error&) {
        if (stage == Fault::INPUT_IPC_READ || stage == Fault::CONTROL_IPC_WRITE || stage == Fault::VIDEO_IPC_WRITE || stage == Fault::BRIDGE_WORKER_READ || stage == Fault::WORKER_PIPE_PAIRING) {
            return Fault::IPC_FAILED;
        }
        return Fault::SYSTEM_FAILURE;
    } catch (...) {
        // Do not classify arbitrary exception strings or transmit their text.
    }
    if (stage == Fault::INPUT_VALIDATION) {
        return Fault::PROTOCOL_REJECTED;
    }
    if (stage == Fault::LIVENESS) {
        return Fault::LIVENESS_TIMEOUT;
    }
    if (stage == Fault::INPUT_BINDING || stage == Fault::INPUT_APPLY || stage == Fault::INPUT_RELEASE) {
        return Fault::INPUT_FAILED;
    }
    return Fault::INTERNAL_FAILURE;
}
} // namespace ultraterimal::agent
