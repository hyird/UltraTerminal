#pragma once

#include <expected>
#include <memory>
#include <optional>

#include <windows.h>

#include <credentialprovider.h>

#include "handle.types.h"
#include "schemas/session.pb.h"

namespace ultraterimal::agent {
// LogonUI apartment owns this listener. Thread-pool waits only post a message;
// I/O completion, identity checks and provider notifications stay on that apartment.
class WindowsLoginInbox {
  public:
    static std::expected<std::unique_ptr<WindowsLoginInbox>, HRESULT> create(HWND window, UINT message, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept;
    ~WindowsLoginInbox();
    bool matches(WPARAM cookie) const noexcept;
    std::expected<std::optional<wire::WindowsLoginDelivery>, HRESULT> receive() noexcept;
    std::expected<UniqueHandle, HRESULT> lease() const noexcept;

  private:
    struct State;
    explicit WindowsLoginInbox(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
