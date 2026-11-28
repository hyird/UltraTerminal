#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "features/desktop/audio.protocol.h"
#include "features/desktop/audio_codec.transport.h"

namespace {
using namespace ultraterimal::agent;

void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

void verifyPcmConversion() {
    // 1. 48 kHz stereo float pass-through with NaN and clipping sanitization.
    std::vector<float> src48Stereo{0.5f, -0.5f, 1.5f, -2.0f, std::numeric_limits<float>::quiet_NaN(), 0.25f};
    std::vector<std::uint8_t> raw(src48Stereo.size() * sizeof(float));
    std::memcpy(raw.data(), src48Stereo.data(), raw.size());
    std::vector<float> out;
    require(convertPcmToStereo48kFloat(raw, {48000, 2, 32, true}, out), "48k stereo float conversion failed");
    require(out.size() == 6, "48k stereo float sample count mismatch");
    require(std::abs(out[0] - 0.5f) < 1e-6f && std::abs(out[1] + 0.5f) < 1e-6f, "48k stereo values mismatch");
    require(out[2] == 1.0f && out[3] == -1.0f, "48k stereo clamping failed");
    require(out[4] == 0.0f && std::abs(out[5] - 0.25f) < 1e-6f, "48k stereo NaN sanitization failed");

    // 2. 48 kHz mono float -> duplicated L/R.
    std::vector<float> srcMono{0.4f, -0.3f};
    raw.resize(srcMono.size() * sizeof(float));
    std::memcpy(raw.data(), srcMono.data(), raw.size());
    out.clear();
    require(convertPcmToStereo48kFloat(raw, {48000, 1, 32, true}, out), "48k mono conversion failed");
    require(out.size() == 4 && std::abs(out[0] - 0.4f) < 1e-6f && std::abs(out[1] - 0.4f) < 1e-6f, "Mono L/R duplication failed");

    // 3. 48 kHz 6-channel int16 -> extracts front L/R.
    std::vector<std::int16_t> src6ch{16384, -16384, 100, 200, 300, 400};
    raw.resize(src6ch.size() * sizeof(std::int16_t));
    std::memcpy(raw.data(), src6ch.data(), raw.size());
    out.clear();
    require(convertPcmToStereo48kFloat(raw, {48000, 6, 16, false}, out), "6ch int16 conversion failed");
    require(out.size() == 2 && std::abs(out[0] - 0.5f) < 1e-4f && std::abs(out[1] + 0.5f) < 1e-4f, "6ch front L/R extraction failed");

    // 4. 44.1 kHz stereo int16 -> resamples to 48 kHz stereo float.
    std::vector<std::int16_t> src441(441 * 2, 8192);
    raw.resize(src441.size() * sizeof(std::int16_t));
    std::memcpy(raw.data(), src441.data(), raw.size());
    out.clear();
    require(convertPcmToStereo48kFloat(raw, {44100, 2, 16, false}, out), "44.1k resampling failed");
    require(out.size() == 480 * 2, "44.1k->48k output frame count mismatch");
    require(std::abs(out[0] - 0.25f) < 1e-3f && std::abs(out.back() - 0.25f) < 1e-3f, "44.1k->48k amplitude mismatch");

    // 5. Rejects invalid format or misaligned byte buffer.
    require(!convertPcmToStereo48kFloat(std::vector<std::uint8_t>(3, 0), {48000, 2, 16, false}, out), "Misaligned buffer accepted");
    require(!convertPcmToStereo48kFloat({}, {0, 2, 32, true}, out), "Zero sample rate accepted");
    require(!convertPcmToStereo48kFloat({}, {48000, 2, 24, false}, out), "Unsupported bit depth accepted");
}

void verifySilenceDetection() {
    std::vector<float> quiet(kAudioFrameSamples * kAudioChannels, 1e-5f);
    require(isAudioSilent(quiet), "Near-zero samples were not treated as silence");
    quiet[100] = -5e-4f;
    require(!isAudioSilent(quiet), "Audible sample was treated as silence");
}

void verifyOpusEncoder() {
    AudioOpusEncoder encoder;
    std::vector<float> tone(kAudioFrameSamples * kAudioChannels);
    for (std::uint32_t i = 0; i < kAudioFrameSamples; ++i) {
        const float sample = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(kAudioSampleRate));
        tone[i * 2] = sample;
        tone[i * 2 + 1] = sample;
    }
    const auto packet1 = encoder.encode(tone);
    require(packet1.has_value() && !packet1->empty() && packet1->size() <= kAudioMaxPacketBytes, "Opus encode failed");
    encoder.reset();
    const auto packet2 = encoder.encode(tone);
    require(packet2.has_value() && !packet2->empty() && packet2->size() <= kAudioMaxPacketBytes, "Opus encode after reset failed");
    std::vector<float> shortTone(100, 0.0f);
    const auto bad = encoder.encode(shortTone);
    require(!bad.has_value(), "Opus accepted wrong frame sample count");
}

} // namespace

int main() {
    try {
        verifyPcmConversion();
        verifySilenceDetection();
        verifyOpusEncoder();
        std::cout << "Desktop audio PCM conversion, silence detection and Opus encoding passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
