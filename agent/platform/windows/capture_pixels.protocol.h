#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraterimal::agent {
struct CapturePixelRect {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
};
// Copy a complete BGRA desktop, excluding source row padding. No previous image is required.
void copyCapturePixels(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                       const void* source, std::size_t pitch);
// Update only the specified clamped BGRA rectangles in an existing complete desktop image.
void copyCapturePixelRects(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                           const void* source, std::size_t pitch, std::span<const CapturePixelRect> rects);
} // namespace ultraterimal::agent
