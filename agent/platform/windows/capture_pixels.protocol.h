#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace ultraterimal::agent {
// Copy a complete BGRA desktop, excluding source row padding. No previous image is required.
void copyCapturePixels(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                       const void* source, std::size_t pitch);
} // namespace ultraterimal::agent
