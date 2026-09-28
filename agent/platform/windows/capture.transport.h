#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "desktop.transport.h"

namespace ultraterimal::agent {
// A recoverable invalidation. Callers must discard retained pixels, encoder
// references and cursor state before creating a new capture instance.
class CaptureReset : public DesktopChanged {
  public:
    explicit CaptureReset(wire::DesktopFault::Code code = wire::DesktopFault::CAPTURE_RESET) : DesktopChanged("Desktop capture requires recreation", code) {}
};
struct CapturedFrame {
    std::uint32_t width = 0, height = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
    std::uint32_t rotation = 0;
    std::uint64_t captureTicks = 0;
    // Empty only for cursor-only updates after the first complete snapshot.
    // Consumers retain the previous pixels; geometry changes require a reset.
    std::vector<std::uint8_t> bgra;
    bool pointerVisible = false;
    std::int32_t pointerX = 0, pointerY = 0;
    std::vector<std::uint8_t> pointerShape;
    std::uint32_t pointerType = 0, pointerWidth = 0, pointerHeight = 0, pointerPitch = 0;
    std::int32_t hotspotX = 0, hotspotY = 0;
};
class DesktopCapture {
  public:
    struct DisplayMode {
        std::uint32_t width, height;
    };
    explicit DesktopCapture(std::uint32_t displayIndex = 0);
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;
    // No new frame returns nullopt; access loss is a failure requiring recreation.
    // The optional buffer is consumed only for a new complete pixel snapshot.
    // Timeout and cursor-only updates leave it intact. The caller must finish
    // using these pixels before calling; on failure discard its old snapshot.
    std::optional<CapturedFrame> nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels = nullptr);
    std::vector<DisplayMode> displayModes() const;
    bool resize(std::uint32_t width, std::uint32_t height);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
