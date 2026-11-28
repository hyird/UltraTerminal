#pragma once
#include <cstdint>
#include <memory>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Diagnostic input marker, never an authentication or authorization signal.
inline constexpr std::uintptr_t kDesktopInputMarker = 0x5554524d;
// `kExclusive` owns interactive session write control and yields briefly while
// a short-lived `kPreempt` action runs. Coordinates are physical pixels in the
// selected output; use and destroy on the creating thread.
class DesktopInput {
  public:
    enum class Ownership { kExclusive,
                           kPreempt };
    DesktopInput(std::int32_t left, std::int32_t top, std::uint32_t width, std::uint32_t height, Ownership ownership = Ownership::kExclusive);
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
