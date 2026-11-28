#include "audio_codec.transport.h"

#include <opus/opus.h>
#include <stdexcept>
#include <string>
#include <utility>

#include "audio.protocol.h"

namespace ultraterimal::agent {

AudioOpusEncoder::AudioOpusEncoder() {
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(static_cast<opus_int32>(kAudioSampleRate), static_cast<int>(kAudioChannels), OPUS_APPLICATION_AUDIO, &error);
    if (!encoder_ || error != OPUS_OK) {
        if (encoder_) {
            opus_encoder_destroy(encoder_);
            encoder_ = nullptr;
        }
        throw std::runtime_error("Cannot create Opus audio encoder: " + std::to_string(error));
    }
    (void)opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(kAudioBitrate)));
    (void)opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    (void)opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_AUTO));
}

AudioOpusEncoder::~AudioOpusEncoder() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
    }
}

AudioOpusEncoder::AudioOpusEncoder(AudioOpusEncoder&& other) noexcept : encoder_(std::exchange(other.encoder_, nullptr)) {}

AudioOpusEncoder& AudioOpusEncoder::operator=(AudioOpusEncoder&& other) noexcept {
    if (this != &other) {
        if (encoder_) {
            opus_encoder_destroy(encoder_);
        }
        encoder_ = std::exchange(other.encoder_, nullptr);
    }
    return *this;
}

std::expected<std::vector<std::uint8_t>, int> AudioOpusEncoder::encode(std::span<const float> stereoSamples) {
    if (!encoder_ || stereoSamples.size() != static_cast<std::size_t>(kAudioFrameSamples) * kAudioChannels) {
        return std::unexpected(OPUS_BAD_ARG);
    }
    std::vector<std::uint8_t> packet(kAudioMaxPacketBytes);
    const opus_int32 bytes = opus_encode_float(encoder_, stereoSamples.data(), static_cast<int>(kAudioFrameSamples),
                                               packet.data(), static_cast<opus_int32>(packet.size()));
    if (bytes <= 0) {
        return std::unexpected(static_cast<int>(bytes));
    }
    packet.resize(static_cast<std::size_t>(bytes));
    return packet;
}

void AudioOpusEncoder::reset() noexcept {
    if (encoder_) {
        (void)opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
    }
}

} // namespace ultraterimal::agent
