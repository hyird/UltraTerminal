#pragma once

#include "video_color.protocol.h"

namespace ultraterimal::agent::detail {
// Internal kernel: caller has validated all extents, even row bounds and CPU/OS AVX2 support.
void convertVideoPixelRowsAvx2(const std::uint8_t* bgra, std::size_t stride, std::uint32_t width, std::uint32_t height, std::uint32_t rowBegin, std::uint32_t rowEnd, VideoPixelFormat format, std::uint8_t* output);
} // namespace ultraterimal::agent::detail
