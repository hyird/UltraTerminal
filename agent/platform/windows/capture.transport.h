#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "desktop.transport.h"
#include "ultraterimal/protocol/message.protocol.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace ultraterimal::agent {
// A recoverable invalidation. Callers must discard retained pixels, encoder
// references and cursor state before creating a new capture instance.
class CaptureReset : public DesktopChanged {
  public:
    explicit CaptureReset(wire::DesktopFault::Code code = wire::DesktopFault::CAPTURE_RESET) : DesktopChanged("Desktop capture requires recreation", code) {}
};
// Local durations with presence: absent means not run; present zero is a valid
// sub-microsecond observation. Stages may overlap; do not sum as end-to-end time.
struct CaptureStageTiming {
    std::optional<std::uint32_t> acquireWaitMicroseconds, mapMicroseconds, rowCopyMicroseconds;
};
// Benchmark diagnostics retain their millisecond fields. Production timing uses
// stage presence instead. Copy dispatch is not GPU completion.
struct CaptureTiming {
    CaptureStageTiming stages;
    // Benchmark-only: force GPU staging, then optionally compare the produced
    // pixels against a full-frame staging readback of the same acquired image.
    bool forceStaging = false;
    bool verifyWithStaging = false;
    bool matchedStaging = false;
    bool desktopImageInSystemMemory = false;
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
    std::uint64_t captureEpoch = 0, sourceSerial = 0;
    std::uint32_t dirtyRowBegin = 0, dirtyRowEnd = 0;
    std::uint64_t changedPixels = 0;
    CaptureStageTiming stageTiming;
    // Empty for cursor updates and deferred GPU snapshots. captureTicks marks
    // a desktop update even when CPU readback was deferred by receiver credit.
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
    // readPixels=false retains only the latest GPU image and pointer metadata.
    // A later readPixels=true materializes it even if DXGI has no new update;
    // every CPU readback copies the complete image into reusable storage.
    // Optional zero-copy pixel consumer invoked while the DXGI surface or staging
    // texture is mapped. Returning true bypasses allocating and copying frame.bgra.
    using DirectPixelConsumer = std::function<bool(std::span<const std::uint8_t> bgra, std::size_t rowPitch,
                                                   std::uint32_t width, std::uint32_t height,
                                                   std::uint32_t rowBegin, std::uint32_t rowEnd,
                                                   std::uint64_t captureEpoch, std::uint64_t sourceSerial,
                                                   bool incremental)>;
    std::optional<CapturedFrame> nextFrame(std::chrono::milliseconds timeout, std::vector<std::uint8_t>* reusablePixels = nullptr, CaptureTiming* timing = nullptr, bool readPixels = true, bool retainTexture = false, const DirectPixelConsumer* directConsumer = nullptr);
    // Borrowed on the capture worker only; finish encoding before nextFrame().
    ID3D11Device* device() const;
    ID3D11Texture2D* texture() const;
    void readTexture(CapturedFrame& frame, CaptureTiming* timing = nullptr);
    std::vector<DisplayMode> displayModes() const;
    bool resize(std::uint32_t width, std::uint32_t height);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
