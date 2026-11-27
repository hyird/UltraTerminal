#include <format>
#include <unistd.h>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
Capabilities detectCapabilities() {
    Capabilities result;
    result.conpty = ::access("/dev/ptmx", R_OK | W_OK) == 0;
    result.mediaFoundationPresent = false;
    result.elevated = ::geteuid() == 0;
    result.processSession = 0;
    result.activeConsoleSession = 0xffffffff;
    result.attachedDisplays = 0;
    result.graphicsError = 0;
    return result;
}

std::string capabilityJson(const Capabilities& c) {
    return std::format(R"({{"platform":"linux-x64","conpty_api":{},"media_foundation_present":{},"elevated":{},"process_session":{},"active_console_session":{},"attached_displays":{},"graphics_error":{},"gateway_connected":false}})",
                       c.conpty, c.mediaFoundationPresent, c.elevated, c.processSession,
                       c.activeConsoleSession == 0xffffffff ? "null" : std::to_string(c.activeConsoleSession), c.attachedDisplays, c.graphicsError);
}
} // namespace ultraterimal::agent
