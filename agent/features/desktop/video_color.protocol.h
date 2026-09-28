#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraterimal::agent {
enum class VideoPixelFormat { kI420,
                              kNv12 };
// BT.601 limited range; output is tightly packed, input may have row padding.
// Invalid dimensions, strides or buffer extents return false without writing.
bool convertVideoPixels(std::span<const std::uint8_t> bgra, std::size_t stride,
                        std::uint32_t width, std::uint32_t height, VideoPixelFormat format,
                        std::span<std::uint8_t> output);
} // namespace ultraterimal::agent
