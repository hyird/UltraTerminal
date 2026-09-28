#include <iostream>
#include <stdexcept>

#include "platform/windows/login_peer.transport.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    using namespace ultraterimal::agent;
    try {
        require(!openWindowsLoginUiPeer(0, 1), "Zero PID accepted");
        require(!openWindowsLoginUiPeer(GetCurrentProcessId(), 0), "Session zero accepted as LogonUI");
        require(!openWindowsLoginUiPeer(GetCurrentProcessId(), 0xffffffffUL), "Invalid session accepted");
        require(!openWindowsLoginServicePeer(0), "Zero service PID accepted");
        DWORD session = 0;
        require(ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE, "Cannot determine test session");
        DWORD before = 0, after = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "Cannot count handles");
        for (int index = 0; index < 100; ++index) {
            require(!openWindowsLoginUiPeer(GetCurrentProcessId(), session), "Test process impersonated LogonUI");
            require(!openWindowsLoginServicePeer(GetCurrentProcessId()), "Test process impersonated Agent service");
        }
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after == before, "Failed peer checks leaked handles");
        std::cout << "Invalid and unrelated login IPC peers rejected without handle leaks; positive SYSTEM peer verification not exercised\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
