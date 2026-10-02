#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <windows.h>

struct ID3D11Device;
struct ID3D11Texture2D;
struct IMFMediaBuffer;
struct IMFAsyncCallback;

namespace ultraterimal::agent {
struct MediaEncoderConfig {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t bitrate;
    std::uint32_t framesPerSecond;
    bool sharp = false;
};
struct MediaEncodedFrame {
    std::vector<std::uint8_t> bytes;
    std::uint64_t timestampMicroseconds;
    bool keyFrame;
};
// Thread-affine H264 hardware adapter. Call only on an encoding worker.
// Media Foundation is optional and dynamically loaded. Startup failure throws;
// frame/rate failures return HRESULT and require recreating the encoder.
class MediaEncoder {
  public:
    explicit MediaEncoder(MediaEncoderConfig config, ID3D11Device* device = nullptr);
    ~MediaEncoder();
    MediaEncoder(const MediaEncoder&) = delete;
    MediaEncoder& operator=(const MediaEncoder&) = delete;
    std::expected<MediaEncodedFrame, HRESULT> encode(std::span<const std::uint8_t> nv12, std::uint64_t timestampMicroseconds, bool forceKeyFrame);
    std::expected<MediaEncodedFrame, HRESULT> encodeTexture(ID3D11Texture2D* bgra, std::uint64_t timestampMicroseconds, bool forceKeyFrame);
    bool gpu() const;
    std::expected<void, HRESULT> setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate);
    std::expected<void, HRESULT> setDetail(bool sharp);

  private:
    struct State;
    std::unique_ptr<State> state_;
    std::expected<MediaEncodedFrame, HRESULT> encodeBuffer(IMFMediaBuffer* buffer, std::uint64_t timestampMicroseconds, bool forceKeyFrame, IMFAsyncCallback* released = nullptr);
};
} // namespace ultraterimal::agent
