#pragma once

#include <cstdint>
#include <span>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Select high-contrast, low-palette detail tiles; never classify or extract text.
// At most 32 tiles, 256 KiB encoded bytes and 256 Ki pixels per static snapshot.
wire::VideoRefinement refineVideoDetails(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);
} // namespace ultraterimal::agent
