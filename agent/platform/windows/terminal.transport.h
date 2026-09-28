#pragma once
#include <cstdint>
#include <string>

namespace ultraterimal::agent {
struct TerminalDiagnostic {
    std::uint32_t exitCode = 0;
    bool markerReceived = false;
};
// Local diagnostic only: runs as the caller and refuses LocalSystem.
TerminalDiagnostic checkLocalTerminal();
} // namespace ultraterimal::agent
