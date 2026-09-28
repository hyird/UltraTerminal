#pragma once

#include <expected>
#include <string>

#include "handle.types.h"

namespace ultraterimal::agent {
inline std::wstring windowsLoginPipeName(DWORD session) { return L"\\\\.\\pipe\\Ultraterimal-Login-" + std::to_wstring(session); }
inline std::wstring windowsLoginEventName(DWORD session) { return L"Global\\Ultraterimal-Login-" + std::to_wstring(session); }
// Retain the returned handle for the whole IPC exchange to prevent PID reuse.
// A live process is only an identity check; it does not grant a login attempt.
std::expected<UniqueHandle, DWORD> openWindowsLoginUiPeer(DWORD processId, DWORD sessionId) noexcept;
std::expected<UniqueHandle, DWORD> openWindowsLoginServicePeer(DWORD processId) noexcept;
} // namespace ultraterimal::agent
