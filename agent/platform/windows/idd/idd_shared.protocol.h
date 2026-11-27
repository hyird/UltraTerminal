#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraterimal::agent::idd {

inline constexpr wchar_t kShmName[] = L"Global\\UltraterimalIddFrameShm";
inline constexpr wchar_t kEventName[] = L"Global\\UltraterimalIddFrameEvent";
inline constexpr wchar_t kHardwareId[] = L"Root\\UltraterimalIdd";
inline constexpr wchar_t kAdapterFriendlyName[] = L"Ultraterimal Virtual Display";

inline constexpr std::uint32_t kShmMagic = 0x55544944u; // "UTID"
inline constexpr std::uint32_t kShmVersion = 1u;
inline constexpr std::uint32_t kMaxWidth = 3840u;
inline constexpr std::uint32_t kMaxHeight = 2160u;
inline constexpr std::size_t kMaxFrameBytes = static_cast<std::size_t>(kMaxWidth) * kMaxHeight * 4u;
inline constexpr std::uint32_t kFormatBgra8 = 87u; // DXGI_FORMAT_B8G8R8A8_UNORM

struct DirtyRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};

struct DirtyBand {
    std::uint32_t rowBegin = 0;
    std::uint32_t rowEnd = 0;
    std::uint64_t changedPixels = 0;
    bool fullFrame = true;
};

inline DirtyBand computeDirtyBand(std::uint32_t width, std::uint32_t height, std::span<const DirtyRect> rects) noexcept {
    const std::uint64_t totalPixels = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || rects.empty() || rects.size() > 64u) {
        return {0u, height, totalPixels, true};
    }
    std::uint32_t minTop = height;
    std::uint32_t maxBottom = 0;
    std::uint64_t area = 0;
    for (const auto& rect : rects) {
        const auto l = static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect.left, 0, width));
        const auto t = static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect.top, 0, height));
        const auto r = static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect.right, 0, width));
        const auto b = static_cast<std::uint32_t>(std::clamp<std::int64_t>(rect.bottom, 0, height));
        if (l < r && t < b) {
            minTop = std::min(minTop, t);
            maxBottom = std::max(maxBottom, b);
            area += static_cast<std::uint64_t>(r - l) * (b - t);
        }
    }
    if (minTop >= maxBottom || area == 0 || area > totalPixels * 3u / 5u) {
        return {0u, height, totalPixels, true};
    }
    const std::uint32_t rowBegin = minTop & ~1u;
    const std::uint32_t rowEnd = std::min(height, (maxBottom + 1u) & ~1u);
    return {rowBegin, rowEnd, std::min(area, totalPixels), false};
}

struct SlotMetadata {
    std::uint64_t frameSeq = 0;
    std::uint64_t qpcTicks = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t format = 0;
    std::uint32_t dirtyRowBegin = 0;
    std::uint32_t dirtyRowEnd = 0;
    std::uint64_t changedPixels = 0;
    std::uint32_t fullFrame = 1;
    std::uint32_t inSystemMemory = 0;
    std::uint32_t acquireMicroseconds = 0;
    std::uint32_t copyMicroseconds = 0;
};

struct SharedHeader {
    std::atomic<std::uint32_t> magic{0};
    std::atomic<std::uint32_t> version{0};
    std::atomic<std::uint32_t> activeSlot{0};
    std::atomic<std::uint32_t> driverActive{0};
    std::atomic<std::uint32_t> inSystemMemory{0};
    std::atomic<std::uint32_t> width{0};
    std::atomic<std::uint32_t> height{0};
    std::atomic<std::uint32_t> reserved{0};
    std::atomic<std::uint64_t> frameSeq{0};
    std::atomic<std::uint64_t> heartbeatQpc{0};
    SlotMetadata slots[2]{};
};

struct SharedRegion {
    SharedHeader header{};
    std::uint8_t pixels[2][kMaxFrameBytes];
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace ultraterimal::agent::idd
