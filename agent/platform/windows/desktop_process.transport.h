#pragma once
#include <cstdint>

#include "handle.types.h"

namespace ultraterimal::agent {
// Holds the expected IPC server process alive as an identity object, preventing
// PID reuse while the desktop worker is connected. Never changes user/session.
class DesktopParent {
  public:
    DesktopParent(std::uint32_t windowsSession, std::uint32_t parentProcess);

  private:
    UniqueHandle process_;
};
} // namespace ultraterimal::agent
