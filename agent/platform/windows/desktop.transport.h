#pragma once
#include <cstdint>
#include <stdexcept>
namespace ultraterimal::agent {
class DesktopChanged : public std::runtime_error {
  public:
    explicit DesktopChanged(const char* message) : std::runtime_error(message) {}
};
// Returns this thread's desktop generation. SYSTEM workers follow Default and
// Winlogon input desktops; ordinary processes remain confined to Default.
// Changes only this thread's desktop, never the user's visible desktop.
std::uint64_t requireInputDesktop();
} // namespace ultraterimal::agent
