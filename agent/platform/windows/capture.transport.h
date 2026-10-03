#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "desktop.transport.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace ultraterimal::agent {
// A recoverable invalidation. Callers must discard retained pixels, encoder
// references and cursor state before creating a new capture instance.
class CaptureReset : public DesktopChanged {
  public:
    explicit CaptureReset(wire::DesktopFault::Code code = wire::DesktopFault::CAPTURE_RESET) : DesktopChanged("Desktop capture requires recreation", code) {}
};
// Optional synchronous diagnostics; zero when the corresponding stage was not run.
// Copy dispatch is not GPU completion: Map includes the synchronization wait.
struct CaptureTiming {
    // Benchmark-only: force GPU staging, then optionally compare the produced
    // pixels against a full-frame staging readback of the same acquired image.
    bool forceStaging = false;
    bool verifyWithStaging = false;
    bool matchedStaging = false;
    bool desktopImageInSystemMemory = false;
    bool partialReadback = false;
    std::uint32_t dirtyRectangles = 0, movedRectangles = 0;
    std::uint64_t changedPixels = 0;
    double metadataMilliseconds = 0;
    double acquireMilliseconds = 0;
    double copyDispatchMilliseconds = 0;
    double mapMilliseconds = 0;
    double rowCopyMilliseconds = 0;
    double verificationStagingMapMilliseconds = 0;
    double verificationStagingRowCopyMilliseconds = 0;
};
struct CapturedFrame {
    std::uint32_t width = 0, height = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
    std::uint32_t rotation = 0;
    std::uint64_t captureTicks = 0;
    // Empty for cursor updates and deferred GPU snapshots. captureTicks marks
    // a desktop update even when CPU readback was deferred by receiver credit.
    std::vector<std::uint8_t> bgra;
    // DXGI damage since the immediately preceding acquired desktop image.
    // Consumers must accumulate damage when coalescing raw snapshots.
    bool damageKnown = false;
    std::vector<wire::CopyRect> moves;
    std::vector<wire::Rectangle> dirty;
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
    // Set previousPixels only when it contains the immediately preceding image
    // returned by this capture, including through cursor-only updates; this
    // allows DXGI move/dirty regions to update that image in place. Timeouts
    // and cursor-only updates leave it intact. Finish using pixels before
    // calling; on failure discard the old snapshot.
    // readPixels=false retains only the latest GPU image and pointer metadata.
    // A later readPixels=true materializes it even if DXGI has no new update;
    // skipped CPU baselines force a complete readback before partial updates.
    std::optional<CapturedFrame> nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels = nullptr, CaptureTiming* timing = nullptr, bool previousPixels = false, bool readPixels = true, bool retainTexture = false);
    // Borrowed on the capture worker only; finish encoding before nextFrame().
    ID3D11Device* device() const;
    ID3D11Texture2D* texture() const;
    void readTexture(CapturedFrame& frame);
    std::vector<DisplayMode> displayModes() const;
    bool resize(std::uint32_t width, std::uint32_t height);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
