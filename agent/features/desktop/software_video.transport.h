#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "video.types.h"

namespace ultraterimal::agent {
// Single-owner synchronous codec adapter. Run on a bounded encoding worker, not an I/O executor.
// Input is borrowed only during encode(); returned Annex B storage is independently owned.
class SoftwareVideoEncoder {
  public:
    explicit SoftwareVideoEncoder(VideoEncoderConfig config);
    ~SoftwareVideoEncoder();
    void setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate);
    void setDetail(bool sharp);
    SoftwareVideoEncoder(const SoftwareVideoEncoder&) = delete;
    SoftwareVideoEncoder& operator=(const SoftwareVideoEncoder&) = delete;
    std::expected<EncodedVideoFrame, VideoEncodeError> encodeI420(std::span<const std::uint8_t> i420,
                                                                  std::uint64_t timestampMicroseconds,
                                                                  bool forceKeyFrame = false);
    std::expected<EncodedVideoFrame, VideoEncodeError> encode(std::span<const std::uint8_t> bgra,
                                                              std::size_t stride,
                                                              std::uint64_t timestampMicroseconds,
                                                              bool forceKeyFrame = false);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
