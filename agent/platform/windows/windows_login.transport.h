#pragma once

#include <filesystem>
#include <string>

#include "login_serialization.transport.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Service-owned, blocking storage operation. Never pass the request to a desktop worker.
wire::WindowsLoginResult windowsLoginSettings(const std::filesystem::path& path, wire::WindowsLoginRequest request);
// Service-owned blocking operation. Only call after authorizing a live control
// session and its LogonUI peer. The result contains no plaintext password.
struct PreparedWindowsLogon {
    WindowsLogonBuffer buffer;
    std::wstring account;
    std::wstring sid;
};
std::expected<PreparedWindowsLogon, HRESULT> prepareSavedWindowsLogon(const std::filesystem::path& path, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept;
} // namespace ultraterimal::agent
