#pragma once
#include <cstdint>
#include <memory>
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
    static bool requiresBackground(const CapturedFrame& frame);
    std::optional<wire::CursorUpdate> update(const CapturedFrame& frame, bool force = false);

  private:
    std::optional<wire::CursorUpdate> previous_;
    // Color shapes do not depend on desktop pixels. Cache their raw bytes so
    // position-only updates avoid rebuilding the same RGBA on every frame.
    std::vector<std::uint8_t> colorShape_;
    std::uint32_t colorPitch_ = 0;
};

// When DXGI Desktop Duplication composites a software cursor directly into the
// desktop texture (PointerPosition.Visible == FALSE, common on virtual displays),
// replace the standard system cursors with a 1x1 transparent cursor so the video
// frame stays cursor-free while forwarding the original shapes for local rendering.
class SystemCursorSuppression {
  public:
    SystemCursorSuppression();
    ~SystemCursorSuppression();
    SystemCursorSuppression(const SystemCursorSuppression&) = delete;
    SystemCursorSuppression& operator=(const SystemCursorSuppression&) = delete;

    bool active() const;
    void applyFallbackCursor(CapturedFrame& frame, std::int32_t desktopLeft, std::int32_t desktopTop);
    bool pollCursorChange(CapturedFrame& frame, std::int32_t desktopLeft, std::int32_t desktopTop);
    void restore();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
