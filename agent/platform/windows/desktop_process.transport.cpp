#include "desktop_process.transport.h"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace ultraterimal::agent {
DesktopParent::DesktopParent(std::uint32_t windowsSession, std::uint32_t parentProcess) {
    DWORD currentSession = 0;
    if (!windowsSession || windowsSession == 0xffffffff || !ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) || currentSession != windowsSession) {
        throw std::runtime_error("Desktop process Windows session mismatch");
    }
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        throw std::runtime_error("Cannot verify desktop process identity");
    }
    UniqueHandle token(rawToken);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) {
        throw std::runtime_error("Invalid desktop process identity");
    }
    std::vector<std::byte> storage(size);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size)) {
        throw std::runtime_error("Cannot read desktop process identity");
    }
    const auto sid = reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid;
    const bool system = IsWellKnownSid(sid, WinLocalSystemSid);
    if (IsWellKnownSid(sid, WinLocalServiceSid) || IsWellKnownSid(sid, WinNetworkServiceSid)) {
        throw std::runtime_error("Unsupported desktop worker identity");
    }
    if (!parentProcess || parentProcess == GetCurrentProcessId()) {
        throw std::runtime_error("Invalid desktop parent process");
    }
    process_.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentProcess));
    if (!process_ || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) {
        throw std::runtime_error("Desktop parent process is unavailable");
    }
    if (system) {
        DWORD parentSession = 0;
        if (!ProcessIdToSessionId(parentProcess, &parentSession) || parentSession != 0 ||
            !OpenProcessToken(process_.get(), TOKEN_QUERY, &rawToken)) {
            throw std::runtime_error("Privileged desktop requires a Session 0 service parent");
        }
        token.reset(rawToken);
        size = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (!size || size > 65536) {
            throw std::runtime_error("Invalid service parent identity");
        }
        storage.resize(size);
        if (!GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size) ||
            !IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(storage.data())->User.Sid, WinLocalSystemSid)) {
            throw std::runtime_error("Privileged desktop parent must be LocalSystem");
        }
    }
}
} // namespace ultraterimal::agent
