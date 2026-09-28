#pragma once
#include <cstdint>
#include <optional>

#include "schemas/session.pb.h"

namespace ultraterimal::protocol {
// Version 1 worker exit status: 0x5544SSCC. The process handle remains usable
// after IPC failure. Ordinary exit codes and Windows crash codes are not faults
// in this format. Worker/service are installed together; legacy exit=1 is valid.
inline bool validDesktopFault(const wire::DesktopFault& fault) {
    return wire::DesktopFault_Stage_IsValid(fault.stage()) && fault.stage() != wire::DesktopFault::STAGE_UNKNOWN &&
           wire::DesktopFault_Code_IsValid(fault.code()) && fault.code() != wire::DesktopFault::CODE_UNKNOWN &&
           fault.windows_session_id() != 0 && fault.windows_session_id() != UINT32_MAX;
}
inline std::uint32_t desktopFailureExitCode(wire::DesktopFault::Stage stage, wire::DesktopFault::Code code) {
    if (!wire::DesktopFault_Stage_IsValid(stage) || stage == wire::DesktopFault::STAGE_UNKNOWN ||
        !wire::DesktopFault_Code_IsValid(code) || code == wire::DesktopFault::CODE_UNKNOWN || stage > 255 || code > 255) {
        return 1;
    }
    return 0x55440000U | (static_cast<std::uint32_t>(stage) << 8) | static_cast<std::uint32_t>(code);
}
inline std::optional<wire::DesktopFault> desktopFailureFromExitCode(std::uint32_t exitCode, std::uint32_t windowsSession, std::uint32_t processId) {
    if ((exitCode & 0xffff0000U) != 0x55440000U || !processId) {
        return std::nullopt;
    }
    wire::DesktopFault fault;
    fault.set_stage(static_cast<wire::DesktopFault::Stage>((exitCode >> 8) & 0xff));
    fault.set_code(static_cast<wire::DesktopFault::Code>(exitCode & 0xff));
    fault.set_windows_session_id(windowsSession);
    fault.set_process_id(processId);
    return validDesktopFault(fault) ? std::optional{fault} : std::nullopt;
}
} // namespace ultraterimal::protocol
