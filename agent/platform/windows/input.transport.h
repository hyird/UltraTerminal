#pragma once
#include <cstdint>
#include <memory>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Diagnostic input marker, never an authentication or authorization signal.
inline constexpr std::uintptr_t kDesktopInputMarker = 0x5554524d;
// One instance owns write control in this Windows session. Use and destroy on
// the creating thread. Coordinates are physical pixels in the selected output.
class DesktopInput {
  public:
    DesktopInput(std::int32_t left, std::int32_t top, std::uint32_t width, std::uint32_t height);
    ~DesktopInput();
    DesktopInput(const DesktopInput&) = delete;
    DesktopInput& operator=(const DesktopInput&) = delete;
    void apply(const wire::InputEvent& event);
    void releaseAll();
    bool needsRebind() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
