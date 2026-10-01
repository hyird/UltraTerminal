#pragma once
#include <cstdint>
#include <optional>
#include <vector>

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
    // Color shapes do not depend on desktop pixels. Cache their raw bytes so
    // position-only updates avoid rebuilding the same RGBA on every frame.
    std::vector<std::uint8_t> colorShape_;
    std::uint32_t colorPitch_ = 0;
};
} // namespace ultraterimal::agent
