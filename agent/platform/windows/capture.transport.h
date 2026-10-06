#pragma once
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "desktop.transport.h"
#include "texture_regions.transport.h"

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
    std::uint64_t captureEpoch = 0, sourceSerial = 0;
    CaptureStageTiming stageTiming;
    DesktopReadbackTag readbackTag(std::uint64_t generation, std::uint64_t composedBase) const {
        return {captureEpoch, sourceSerial, generation, composedBase, width, height, desktopLeft, desktopTop, rotation};
    }
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
    // Call on the capture worker after nextFrame(..., retainTexture=true) (or
    // readPixels=false), BEFORE its next call. Prefer known damage ROIs; no
    // optional whole-screen comparison is required by this path. The tag must
    // describe the retained source and the owner's current composed canvas.
    std::expected<DesktopReadbackSubmit, HRESULT> submitTextureRegions(const DesktopReadbackTag& tag, std::span<const DesktopReadbackRegion> regions);
    // One DO_NOT_WAIT check. Any identity/base/geometry mismatch retires work;
    // Ready output is returned only for expected AND the still-current source.
    std::expected<DesktopReadbackCompletion, HRESULT> tryCompleteTextureRegions(const DesktopReadbackTag& expected);
    void retireTextureRegions();
    // Explicitly queues an independent GPU baseline for this retained source
    // and a new composed base. kQueued is not GPU completion. Optional failure
    // is Unavailable; no acquire/readback/legacy remember advances this key.
    std::expected<DesktopTextureBaselineResult, HRESULT> rememberTextureBaseline(const DesktopReadbackTag& tag);
    std::expected<DesktopReadbackSubmit, HRESULT> submitTextureComparison(const DesktopReadbackTag& currentTag, const DesktopReadbackTag& expectedBaselineTag, std::span<const DesktopReadbackRegion> regions);
    // Private-output rejection is the owner's responsibility; clearing retires
    // pending comparison, never frees its physical slot before a real drain.
    void clearTextureBaseline();
    void readTexture(CapturedFrame& frame, CaptureTiming* timing = nullptr);
    // Sparse rows do not establish a complete CPU baseline for nextFrame().
    std::uint64_t readTextureRegions(CapturedFrame& frame, std::span<const wire::Rectangle> regions, CaptureTiming* timing = nullptr);
    std::optional<std::vector<std::uint32_t>> changedTextureTiles();
    void rememberTexture();
    std::vector<DisplayMode> displayModes() const;
    bool resize(std::uint32_t width, std::uint32_t height);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
