#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

struct OpusEncoder;

namespace ultraterimal::agent {

// RAII Opus encoder for 48 kHz stereo 20 ms float32 frames.
class AudioOpusEncoder {
  public:
    AudioOpusEncoder();
    ~AudioOpusEncoder();
    AudioOpusEncoder(const AudioOpusEncoder&) = delete;
    AudioOpusEncoder& operator=(const AudioOpusEncoder&) = delete;
    AudioOpusEncoder(AudioOpusEncoder&& other) noexcept;
    AudioOpusEncoder& operator=(AudioOpusEncoder&& other) noexcept;

    std::expected<std::vector<std::uint8_t>, int> encode(std::span<const float> stereoSamples);
    void reset() noexcept;

  private:
    OpusEncoder* encoder_ = nullptr;
};

} // namespace ultraterimal::agent
