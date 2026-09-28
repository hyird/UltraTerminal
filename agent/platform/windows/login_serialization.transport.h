#pragma once

#include <expected>
#include <span>
#include <string_view>

#include <windows.h>

#include <credentialprovider.h>

namespace ultraterimal::agent {
// Owns a protected logon buffer until LogonUI takes ownership. Never copy it.
class WindowsLogonBuffer {
  public:
    WindowsLogonBuffer(BYTE* data, ULONG size, ULONG package) noexcept;
    ~WindowsLogonBuffer();
    WindowsLogonBuffer(const WindowsLogonBuffer&) = delete;
    WindowsLogonBuffer& operator=(const WindowsLogonBuffer&) = delete;
    WindowsLogonBuffer(WindowsLogonBuffer&& other) noexcept;
    WindowsLogonBuffer& operator=(WindowsLogonBuffer&& other) noexcept;
    const BYTE* data() const noexcept { return data_; }
    ULONG size() const noexcept { return size_; }
    ULONG package() const noexcept { return package_; }
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION release(const CLSID& provider) noexcept;

  private:
    void clear() noexcept;
    BYTE* data_ = nullptr;
    ULONG size_ = 0;
    ULONG package_ = 0;
};

// CPUS_CREDUI (including UAC) and password-change scenarios are rejected.
std::expected<WindowsLogonBuffer, HRESULT> serializeWindowsLogon(std::wstring_view domain, std::wstring_view username,
                                                                 std::wstring_view password, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept;
std::expected<WindowsLogonBuffer, HRESULT> importWindowsLogon(std::span<const BYTE> bytes, ULONG package,
                                                              CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept;
} // namespace ultraterimal::agent
