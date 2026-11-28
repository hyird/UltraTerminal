#include "audio.protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ultraterimal::agent {
namespace {

float sanitizeSample(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

float readPcmSample(std::span<const std::uint8_t> input, std::size_t frameIndex, std::size_t channelIndex,
                    const AudioPcmFormat& format, std::size_t frameStride) noexcept {
    const std::size_t bytesPerSample = format.bitsPerSample / 8;
    const std::size_t offset = frameIndex * frameStride + channelIndex * bytesPerSample;
    if (format.isFloat) {
        float value = 0.0f;
        std::memcpy(&value, input.data() + offset, sizeof(value));
        return sanitizeSample(value);
    }
    std::int16_t sample = 0;
    std::memcpy(&sample, input.data() + offset, sizeof(sample));
    return static_cast<float>(sample) / 32768.0f;
}

} // namespace

bool convertPcmToStereo48kFloat(std::span<const std::uint8_t> input, const AudioPcmFormat& format, std::vector<float>& output) {
    if (format.sampleRate == 0 || format.sampleRate > 384000 || format.channels == 0 || format.channels > 16) {
        return false;
    }
    if (format.isFloat ? format.bitsPerSample != 32 : format.bitsPerSample != 16) {
        return false;
    }
    const std::size_t bytesPerSample = format.bitsPerSample / 8;
    const std::size_t frameStride = static_cast<std::size_t>(format.channels) * bytesPerSample;
    if (input.size() % frameStride != 0) {
        return false;
    }
    if (input.empty()) {
        return true;
    }
    const std::size_t inputFrames = input.size() / frameStride;
    if (format.sampleRate == kAudioSampleRate && format.channels == 2 && format.isFloat) {
        const std::size_t base = output.size();
        output.resize(base + inputFrames * 2);
        std::memcpy(output.data() + base, input.data(), input.size());
        for (std::size_t i = base; i < output.size(); ++i) {
            output[i] = sanitizeSample(output[i]);
        }
        return true;
    }

    const std::size_t rightChannel = format.channels >= 2 ? 1 : 0;
    if (format.sampleRate == kAudioSampleRate) {
        const std::size_t base = output.size();
        output.resize(base + inputFrames * 2);
        for (std::size_t i = 0; i < inputFrames; ++i) {
            output[base + i * 2] = readPcmSample(input, i, 0, format, frameStride);
            output[base + i * 2 + 1] = readPcmSample(input, i, rightChannel, format, frameStride);
        }
        return true;
    }

    const std::size_t outputFrames = static_cast<std::size_t>(
        (static_cast<std::uint64_t>(inputFrames) * kAudioSampleRate + format.sampleRate - 1) / format.sampleRate);
    if (outputFrames == 0) {
        return true;
    }
    const std::size_t base = output.size();
    output.resize(base + outputFrames * 2);
    const double step = static_cast<double>(format.sampleRate) / static_cast<double>(kAudioSampleRate);
    for (std::size_t outIdx = 0; outIdx < outputFrames; ++outIdx) {
        const double srcPos = static_cast<double>(outIdx) * step;
        const std::size_t idx0 = std::min(static_cast<std::size_t>(srcPos), inputFrames - 1);
        const std::size_t idx1 = std::min(idx0 + 1, inputFrames - 1);
        const float frac = static_cast<float>(srcPos - static_cast<double>(idx0));
        const float l0 = readPcmSample(input, idx0, 0, format, frameStride);
        const float l1 = readPcmSample(input, idx1, 0, format, frameStride);
        const float r0 = readPcmSample(input, idx0, rightChannel, format, frameStride);
        const float r1 = readPcmSample(input, idx1, rightChannel, format, frameStride);
        output[base + outIdx * 2] = l0 + (l1 - l0) * frac;
        output[base + outIdx * 2 + 1] = r0 + (r1 - r0) * frac;
    }
    return true;
}

bool isAudioSilent(std::span<const float> stereoSamples, float threshold) noexcept {
    for (const float sample : stereoSamples) {
        if (std::abs(sample) >= threshold) {
            return false;
        }
    }
    return true;
}

} // namespace ultraterimal::agent
