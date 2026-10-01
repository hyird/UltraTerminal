#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <windows.h>

#include <dxgi1_2.h>

namespace ultraterimal::agent {
// Validate DXGI move destinations and dirty regions. The mapped desktop is
// already the final image; all destinations can be copied from it, including
// overlapping moves. Invalid metadata requires a full-frame readback.
std::optional<std::uint64_t> collectCaptureRegions(std::uint32_t width, std::uint32_t height,
                                                   std::span<const DXGI_OUTDUPL_MOVE_RECT> moved, std::span<const RECT> dirty,
                                                   std::vector<RECT>& destinations);
// An empty destination list means a complete readback. Partial readback
// requires the target to contain the preceding complete image.
void copyCapturePixels(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                       const void* source, std::size_t pitch, std::span<const RECT> destinations = {});
} // namespace ultraterimal::agent
