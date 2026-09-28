#pragma once
#include <cstdint>
#include <string_view>

#include "handle.types.h"

namespace ultraterimal::agent {
// Best-effort bounded metadata in Windows Application log (source
// UltraterimalDesktop, event 1001). Never pass credentials or desktop content.
bool reportDesktopFailure(std::string_view stage, std::string_view message, std::string_view session = {}) noexcept;
// Holds the expected IPC server process alive as an identity object, preventing
// PID reuse while the desktop worker is connected. Never changes user/session.
class DesktopParent {
  public:
    DesktopParent(std::uint32_t windowsSession, std::uint32_t parentProcess);

  private:
    UniqueHandle process_;
};
} // namespace ultraterimal::agent
