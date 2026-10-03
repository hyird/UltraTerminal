#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "video.types.h"

namespace ultraterimal::agent {
std::expected<VideoFrameDescription, VideoEncodeError> describeVideoFrame(wire::VideoCodec codec, std::span<const std::uint8_t> bytes, const std::string& previousCodec, bool cleanPoint);
// Independent, embedded software decoders verify the first hardware baseline.
bool verifyVideoBaseline(wire::VideoCodec codec, std::span<const std::uint8_t> bytes, std::uint32_t width, std::uint32_t height);
} // namespace ultraterimal::agent
