#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include "video.types.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace ultraterimal::agent {
// Owns encoder selection and failover on one encoding worker. A failed hardware
// encoder is not retried until a new desktop encoder is constructed.
class VideoEncoder {
  public:
    explicit VideoEncoder(VideoEncoderConfig config, ID3D11Device* device = nullptr);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    void setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate);
    void setDetail(bool sharp);
    bool hardware() const;
    bool gpu() const;
    std::expected<EncodedVideoFrame, VideoEncodeError> encodeTexture(ID3D11Texture2D* bgra, std::uint64_t timestampMicroseconds, bool forceKeyFrame = false);
    std::expected<EncodedVideoFrame, VideoEncodeError> encode(std::span<const std::uint8_t> bgra, std::size_t stride, std::uint64_t timestampMicroseconds, bool forceKeyFrame = false);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
