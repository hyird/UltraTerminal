#pragma once
#include <cstdint>

#include "desktop_failure.error.h"
namespace ultraterimal::agent {
class DesktopChanged : public DesktopFailure {
  public:
    explicit DesktopChanged(const char* message, wire::DesktopFault::Code code = wire::DesktopFault::DESKTOP_UNAVAILABLE) : DesktopFailure(code, message) {}
};
// Returns this thread's desktop generation. SYSTEM workers follow Default and
// Winlogon input desktops; ordinary processes remain confined to Default.
// Changes only this thread's desktop, never the user's visible desktop.
std::uint64_t requireInputDesktop();
} // namespace ultraterimal::agent
