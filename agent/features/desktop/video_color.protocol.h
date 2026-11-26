#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraterimal::agent {
enum class VideoPixelFormat { kI420,
                              kNv12 };
enum class VideoPixelBackend { kAutomatic,
                               kSse2,
                               kAvx2 };
// CPU AVX2 plus OS-enabled XMM/YMM state; safe to call on baseline x64 CPUs.
bool videoPixelAvx2Available();
// BT.601 limited range; output is tightly packed, input may have row padding.
// Invalid extents/backend or unavailable forced AVX2 returns false without writing.
// Automatic dispatch keeps SSE2 as the baseline; explicit paths serve validation.
// Row-slice overload converts [rowBegin, rowEnd) on even row boundaries into
// disjoint luma/chroma plane regions so caller-owned workers can run in parallel.
bool convertVideoPixelRows(std::span<const std::uint8_t> bgra, std::size_t stride,
                           std::uint32_t width, std::uint32_t height,
                           std::uint32_t rowBegin, std::uint32_t rowEnd,
                           VideoPixelFormat format, std::span<std::uint8_t> output,
                           VideoPixelBackend backend = VideoPixelBackend::kAutomatic);
bool convertVideoPixels(std::span<const std::uint8_t> bgra, std::size_t stride,
                        std::uint32_t width, std::uint32_t height, VideoPixelFormat format,
                        std::span<std::uint8_t> output, VideoPixelBackend backend = VideoPixelBackend::kAutomatic);
} // namespace ultraterimal::agent
