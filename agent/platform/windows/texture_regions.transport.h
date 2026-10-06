#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <windows.h>

#include "ultraterimal/protocol/message.protocol.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace ultraterimal::agent {
// Local snapshot identity, not a wire message. ComPtr ownership alone does not
// freeze a mutable source. Submit the Copy before the next capture overwrites it.
struct DesktopReadbackTag {
    std::uint64_t captureEpoch = 0, sourceSerial = 0, generation = 0, composedBase = 0;
    std::uint32_t width = 0, height = 0;
    std::int32_t desktopLeft = 0, desktopTop = 0;
    std::uint32_t rotation = 0;
    bool operator==(const DesktopReadbackTag&) const = default;
};
struct DesktopReadbackRegion {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
};
struct DesktopReadbackTiming {
    std::optional<std::uint32_t> copyDispatchMicroseconds, mapMicroseconds, rowCopyMicroseconds;
    // Local submit-to-observed-ready duration (includes scheduling/queueing),
    // NOT GPU execution time. Absent for Pending and work that never ran.
    std::optional<std::uint32_t> readbackReadyMicroseconds;
    // Fused output-to-staging bytes actually queued (128-byte mask header +
    // requested packed BGRA), not allocation capacity or GPU execution time.
    std::optional<std::uint64_t> gpuBytes;
};
enum class DesktopReadbackSubmit { kSubmitted,
                                   kBusy,
                                   kUnavailable };
enum class DesktopReadbackStatus { kIdle,
                                   kPending,
                                   kReady,
                                   kRetired };
struct DesktopReadbackComparison {
    DesktopReadbackTag baselineTag;
    std::vector<std::uint32_t> changed;
};
enum class DesktopTextureBaselineStatus { kQueued,
                                          kBusy,
                                          kUnavailable };
struct DesktopTextureBaselineResult {
    DesktopTextureBaselineStatus status = DesktopTextureBaselineStatus::kUnavailable;
    std::optional<std::uint32_t> copyDispatchMicroseconds;
    std::uint64_t copiedBytes = 0;
};
struct DesktopReadbackResult {
    DesktopReadbackTag tag;
    std::vector<DesktopReadbackRegion> regions;
    // Packed BGRA rows concatenated in regions order, with no atlas padding.
    std::vector<std::uint8_t> bgra;
    std::uint64_t pixels = 0;
    // Exact RGB equality only inside each corresponding ROI (alpha ignored).
    // All requested pixels, including unchanged ROIs, are frozen in bgra.
    // No full-canvas/damage coverage proof is implied.
    std::optional<DesktopReadbackComparison> comparison;
    // Checks all bounds/storage before writing; untouched pixels stay intact.
    bool applyTo(std::span<std::uint8_t> destination) const;
};
struct DesktopReadbackCompletion {
    DesktopReadbackStatus status = DesktopReadbackStatus::kIdle;
    DesktopReadbackTiming timing;
    std::optional<DesktopReadbackResult> result;
};
// Capture-worker owned D3D resources. The reference advances only after the
// owner submits an image. S_FALSE means optional GPU comparison is unavailable.
class DesktopTextureRegions {
  public:
    explicit DesktopTextureRegions(ID3D11Device* device);
    ~DesktopTextureRegions();
    DesktopTextureRegions(const DesktopTextureRegions&) = delete;
    DesktopTextureRegions& operator=(const DesktopTextureRegions&) = delete;
    // Exactly one physical optional slot. Busy includes retired work until a
    // tryComplete observes completion. No Flush, synchronous Map or retry loop.
    std::expected<DesktopReadbackSubmit, HRESULT> submit(ID3D11Texture2D* texture, const DesktopReadbackTag& tag, std::span<const DesktopReadbackRegion> regions);
    std::expected<DesktopReadbackCompletion, HRESULT> tryComplete();
    void retire();
    // Shares submit's sole physical slot. Copy all requested source ROIs before
    // the next overwrite; one fused output/staging buffer, one nonblocking Map.
    std::expected<DesktopReadbackSubmit, HRESULT> submitComparison(ID3D11Texture2D* texture, const DesktopReadbackTag& currentTag, const DesktopReadbackTag& expectedBaselineTag, std::span<const DesktopReadbackRegion> regions);
    // Queues an independent full GPU copy, never CPU readback/completion proof.
    // Only this explicit new composed-base commit makes comparison available.
    std::expected<DesktopTextureBaselineResult, HRESULT> rememberBaseline(ID3D11Texture2D* texture, const DesktopReadbackTag& tag);
    // Invalidates the key and retires comparison output; slot still must drain.
    void invalidateBaseline();
    HRESULT remember(ID3D11Texture2D* texture);
    std::expected<std::vector<std::uint32_t>, HRESULT> changes(ID3D11Texture2D* texture);
    // An atlas holds at most 32 128x64 tiles. Only the requested BGRA rows are
    // written; other bytes remain untouched and are not a complete CPU image.
    std::expected<std::uint64_t, HRESULT> read(ID3D11Texture2D* texture, std::span<const wire::Rectangle> regions, std::span<std::uint8_t> bgra, DesktopReadbackTiming* timing = nullptr);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
