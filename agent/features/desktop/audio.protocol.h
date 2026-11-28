#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ultraterimal::agent {

inline constexpr std::uint32_t kAudioSampleRate = 48000;
inline constexpr std::uint32_t kAudioChannels = 2;
inline constexpr std::uint32_t kAudioFrameSamples = 960; // 20 ms at 48 kHz
inline constexpr std::uint32_t kAudioFrameDurationMicroseconds = 20000;
inline constexpr std::uint32_t kAudioBitrate = 96000;
inline constexpr std::size_t kAudioMaxPacketBytes = 4096;

struct AudioPcmFormat {
    std::uint32_t sampleRate = kAudioSampleRate;
    std::uint16_t channels = kAudioChannels;
    std::uint16_t bitsPerSample = 32;
    bool isFloat = true;
};

// Converts interleaved PCM frames into interleaved 48 kHz stereo float32 samples
// and appends them to output.
bool convertPcmToStereo48kFloat(std::span<const std::uint8_t> input, const AudioPcmFormat& format, std::vector<float>& output);

// Returns true when every sample's absolute amplitude is below threshold.
bool isAudioSilent(std::span<const float> stereoSamples, float threshold = 1e-4f) noexcept;

} // namespace ultraterimal::agent
