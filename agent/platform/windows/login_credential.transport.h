#pragma once

#include <memory>
#include <string>

#include "handle.types.h"
#include "login_serialization.transport.h"

namespace ultraterimal::agent {
inline constexpr CLSID kWindowsLoginProviderId{0x1d064384, 0xa0e1, 0x4534, {0x92, 0x16, 0x96, 0xc1, 0x9c, 0xdb, 0xba, 0x3d}};

// Provider-owned single-use authorization and protected data. Revocation erases
// the buffer even when LogonUI retains a reference to an old credential tile.
class WindowsLoginAttempt {
  public:
    explicit WindowsLoginAttempt(ULONGLONG expiresAt, UniqueHandle servicePipe = UniqueHandle{});
    ~WindowsLoginAttempt();
    WindowsLoginAttempt(const WindowsLoginAttempt&) = delete;
    WindowsLoginAttempt& operator=(const WindowsLoginAttempt&) = delete;
    bool available() const noexcept;
    bool bind(WindowsLogonBuffer buffer) noexcept;
    WindowsLogonBuffer take() noexcept;
    void cancel() noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

// The returned COM object owns the protected buffer. No plaintext is retained.
HRESULT createWindowsLoginCredential(WindowsLogonBuffer buffer, std::wstring account, std::wstring sid,
                                     std::shared_ptr<WindowsLoginAttempt> attempt, ICredentialProviderCredential2** credential) noexcept;
} // namespace ultraterimal::agent
