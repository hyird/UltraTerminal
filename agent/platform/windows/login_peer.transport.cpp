#include "login_peer.transport.h"

#include <array>
#include <string>
#include <utility>

namespace ultraterimal::agent {
namespace {
struct ServiceHandle {
    SC_HANDLE value;
    ~ServiceHandle() {
        if (value) {
            CloseServiceHandle(value);
        }
    }
};
std::expected<UniqueHandle, DWORD> systemProcess(DWORD processId, DWORD sessionId) noexcept {
    if (!processId || sessionId == 0xffffffffUL) {
        return std::unexpected(ERROR_INVALID_PARAMETER);
    }
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
    if (!process) {
        return std::unexpected(GetLastError());
    }
    if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
        return std::unexpected(ERROR_PROCESS_ABORTED);
    }
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) {
        return std::unexpected(GetLastError());
    }
    UniqueHandle token(rawToken);
    DWORD tokenSession = 0, used = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &tokenSession, sizeof(tokenSession), &used)) {
        return std::unexpected(GetLastError());
    }
    alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user{};
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), static_cast<DWORD>(user.size()), &used)) {
        return std::unexpected(GetLastError());
    }
    if (tokenSession != sessionId || !IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, WinLocalSystemSid)) {
        return std::unexpected(ERROR_ACCESS_DENIED);
    }
    return process;
}
} // namespace
std::expected<UniqueHandle, DWORD> openWindowsLoginUiPeer(DWORD processId, DWORD sessionId) noexcept {
    if (!sessionId || sessionId == 0xffffffffUL) {
        return std::unexpected(ERROR_INVALID_PARAMETER);
    }
    auto process = systemProcess(processId, sessionId);
    if (!process) {
        return std::unexpected(process.error());
    }
    try {
        std::array<wchar_t, 32768> image{};
        DWORD length = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process->get(), 0, image.data(), &length)) {
            return std::unexpected(GetLastError());
        }
        std::array<wchar_t, MAX_PATH + 1> system{};
        const auto systemLength = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!systemLength || systemLength >= system.size()) {
            return std::unexpected(ERROR_BAD_PATHNAME);
        }
        const auto expected = std::wstring(system.data(), systemLength) + L"\\LogonUI.exe";
        if (CompareStringOrdinal(image.data(), static_cast<int>(length), expected.data(), static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL) {
            return std::unexpected(ERROR_ACCESS_DENIED);
        }
        return std::move(*process);
    } catch (...) {
        return std::unexpected(ERROR_NOT_ENOUGH_MEMORY);
    }
}
std::expected<UniqueHandle, DWORD> openWindowsLoginServicePeer(DWORD processId) noexcept {
    auto process = systemProcess(processId, 0);
    if (!process) {
        return std::unexpected(process.error());
    }
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
        return std::unexpected(GetLastError());
    }
    ServiceHandle service{OpenServiceW(manager.value, L"UltraterimalAgent", SERVICE_QUERY_STATUS)};
    if (!service.value) {
        return std::unexpected(GetLastError());
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD used = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &used)) {
        return std::unexpected(GetLastError());
    }
    if (status.dwCurrentState != SERVICE_RUNNING || status.dwProcessId != processId ||
        WaitForSingleObject(process->get(), 0) != WAIT_TIMEOUT) {
        return std::unexpected(ERROR_ACCESS_DENIED);
    }
    return std::move(*process);
}
} // namespace ultraterimal::agent
