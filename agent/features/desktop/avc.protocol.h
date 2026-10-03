#pragma once
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "video.types.h"

namespace ultraterimal::agent {
// Validate Annex B framing and independently decodable IDRs; codec is derived
// from SPS bytes, never assumed from the encoder's requested profile.
std::expected<VideoFrameDescription, VideoEncodeError> describeAvcFrame(std::span<const std::uint8_t> bytes, const std::string& previousCodec);
} // namespace ultraterimal::agent
