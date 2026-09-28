#pragma once
#include <optional>

#include "capture.transport.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Converts DXGI shapes to straight RGBA. XOR pixels are resolved against the
// same captured desktop. Wire x/y are the hotspot in output-local coordinates.
// Empty pixel data means only position/visibility changed; force resends shape.
class DesktopCursor {
  public:
    std::optional<wire::CursorUpdate> update(const CapturedFrame& frame, bool force = false);

  private:
    std::optional<wire::CursorUpdate> previous_;
};
} // namespace ultraterimal::agent
