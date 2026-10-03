#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
struct VideoEncoderConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bitrate = 4000000;
    std::uint32_t framesPerSecond = 30;
    bool sharp = false;
    std::vector<wire::VideoCodec> codecs{wire::AVC};
};
enum class VideoEncodeError { kInvalidPixels,
                              kInvalidTimestamp,
                              kCodecFailure,
                              kFrameSkipped,
                              kFrameTooLarge,
                              kGpuUnavailable };
struct EncodedVideoFrame {
    std::vector<std::uint8_t> annexB;
    std::string codec; // WebCodecs codec string derived from emitted headers.
    std::uint64_t timestampMicroseconds = 0;
    bool keyFrame = false;
    bool baselineReset = false; // Encoder failover or codec change requires a new generation.
};
struct VideoFrameDescription {
    std::string codec;
    bool keyFrame = false;
};
} // namespace ultraterimal::agent
