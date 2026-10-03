#include "video.transport.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>

#include "codecs.protocol.h"
#include "platform/windows/media_encoder.transport.h"
#include "software_video.transport.h"
#include "video_color.protocol.h"

namespace ultraterimal::agent {
struct VideoEncoder::State {
    VideoEncoderConfig config;
    std::unique_ptr<MediaEncoder> media;
    std::unique_ptr<SoftwareVideoEncoder> software;
    std::vector<std::uint8_t> nv12;
    std::optional<std::uint64_t> lastTimestamp;
    std::string codec;
    bool baselinePending = false;
    bool hardwareValidated = false;
    bool refreshRebuilt = false;
    wire::VideoCodec hardwareCodec = wire::AVC;
    ID3D11Device* device = nullptr; // Capture owns it until this encoder is destroyed.

    bool initialize(wire::VideoCodec family, bool preferGpu) {
        media.reset();
        hardwareValidated = false;
        hardwareCodec = family;
        const MediaEncoderConfig settings{config.width, config.height, config.bitrate, config.framesPerSecond, config.sharp, family};
        if (device && preferGpu) {
            try {
                media = std::make_unique<MediaEncoder>(settings, device);
            } catch (const std::runtime_error&) {
            }
        }
        if (!media) {
            try {
                media = std::make_unique<MediaEncoder>(settings);
            } catch (const std::runtime_error&) {
                return false;
            }
        }
        if (!media->gpu()) {
            nv12.resize(static_cast<std::size_t>(config.width) * config.height * 3 / 2);
        }
        return true;
    }

    void fallback() {
        media.reset();
        std::vector<std::uint8_t>().swap(nv12);
        software = std::make_unique<SoftwareVideoEncoder>(config);
        baselinePending = !codec.empty();
    }
    bool fallbackHardware(bool preferGpu) {
        if (hardwareCodec != wire::AVC && initialize(wire::AVC, preferGpu)) {
            baselinePending = !codec.empty();
            return true;
        }
        fallback();
        return false;
    }
    std::optional<EncodedVideoFrame> acceptHardware(std::expected<MediaEncodedFrame, HRESULT> result, std::uint64_t timestamp, bool keyRequired) {
        if (result && result->timestampMicroseconds == timestamp) {
            auto description = describeVideoFrame(hardwareCodec, result->bytes, codec, result->keyFrame);
            const bool needsValidation = !hardwareValidated || keyRequired || (description && codec != description->codec) || (hardwareCodec == wire::AV1 && result->keyFrame);
            if (description && description->keyFrame == result->keyFrame && (!keyRequired || description->keyFrame) &&
                (!needsValidation || (description->keyFrame && verifyVideoBaseline(hardwareCodec, result->bytes, config.width, config.height)))) {
                hardwareValidated = true;
                const bool changed = baselinePending || (!codec.empty() && codec != description->codec);
                baselinePending = false;
                codec = description->codec;
                lastTimestamp = timestamp;
                return EncodedVideoFrame{std::move(result->bytes), codec, timestamp, description->keyFrame, changed};
            }
        }
        return std::nullopt;
    }
    bool withinLatencyBudget(std::chrono::steady_clock::time_point started) const {
        return hardwareCodec == wire::AVC || std::chrono::steady_clock::now() - started <=
                                                 std::chrono::microseconds(std::max(10000u, 1000000u / config.framesPerSecond));
    }
};
VideoEncoder::VideoEncoder(VideoEncoderConfig config, ID3D11Device* device) : state_(std::make_unique<State>()) {
    const auto pixels = static_cast<std::uint64_t>(config.width) * config.height;
    if (config.width < 16 || config.height < 16 || config.width > 4096 || config.height > 4096 ||
        config.width % 2 || config.height % 2 || pixels > 4096 * 2160 || !config.framesPerSecond || config.framesPerSecond > 120 ||
        config.bitrate < 64000 || config.bitrate > 100000000) {
        throw std::invalid_argument("Invalid video configuration");
    }
    state_->config = config;
    state_->device = device;
    for (const auto family : {wire::AV1, wire::HEVC, wire::AVC}) {
        if ((family == wire::AVC || std::find(config.codecs.begin(), config.codecs.end(), family) != config.codecs.end()) && state_->initialize(family, true)) {
            return;
        }
    }
    state_->fallback();
}
VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::hardware() const { return state_->media != nullptr; }
bool VideoEncoder::gpu() const { return state_->media && state_->media->gpu(); }
std::expected<EncodedVideoFrame, VideoEncodeError> VideoEncoder::encodeTexture(ID3D11Texture2D* bgra, std::uint64_t timestamp, bool forceKeyFrame) {
    auto& state = *state_;
    if (!gpu()) {
        return std::unexpected(VideoEncodeError::kGpuUnavailable);
    }
    if (timestamp > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()) / 10 || (state.lastTimestamp && timestamp <= *state.lastTimestamp)) {
        return std::unexpected(VideoEncodeError::kInvalidTimestamp);
    }
    const bool keyRequired = forceKeyFrame || state.codec.empty() || state.baselinePending;
    const auto started = std::chrono::steady_clock::now();
    auto encoded = state.media->encodeTexture(bgra, timestamp, keyRequired);
    if (state.withinLatencyBudget(started)) {
        if (auto result = state.acceptHardware(std::move(encoded), timestamp, keyRequired)) {
            return std::move(*result);
        }
    }
    // Some hardware encoders emit invalid forced refreshes after rate changes.
    // A fresh transform can restore its independently decodable first frame.
    if (bgra && keyRequired && !state.codec.empty() && !state.refreshRebuilt) {
        state.refreshRebuilt = true;
        if (state.initialize(state.hardwareCodec, true) && state.media->gpu()) {
            state.baselinePending = true;
            if (auto result = state.acceptHardware(state.media->encodeTexture(bgra, timestamp, true), timestamp, true)) {
                return std::move(*result);
            }
        }
    }
    if (state.fallbackHardware(true) && state.media->gpu()) {
        if (auto result = state.acceptHardware(state.media->encodeTexture(bgra, timestamp, true), timestamp, true)) {
            return std::move(*result);
        }
        state.fallback();
    }
    return std::unexpected(VideoEncodeError::kGpuUnavailable); // Caller reads back and re-encodes this same snapshot.
}
void VideoEncoder::setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate) {
    if (!framesPerSecond || framesPerSecond > 120 || bitrate < 64000 || bitrate > 100000000) {
        throw std::invalid_argument("Invalid adaptive video rate");
    }
    state_->config.framesPerSecond = framesPerSecond;
    state_->config.bitrate = bitrate;
    if (state_->media && !state_->media->setRate(framesPerSecond, bitrate)) {
        state_->fallbackHardware(state_->media->gpu());
    }
    if (state_->software) {
        state_->software->setRate(framesPerSecond, bitrate);
    }
}
void VideoEncoder::setDetail(bool sharp) {
    if (state_->config.sharp != sharp) {
        state_->hardwareValidated = false;
    }
    state_->config.sharp = sharp;
    if (state_->media && !state_->media->setDetail(sharp)) {
        state_->fallbackHardware(state_->media->gpu());
    }
    if (state_->software) {
        state_->software->setDetail(sharp);
    }
}
std::expected<EncodedVideoFrame, VideoEncodeError> VideoEncoder::encode(std::span<const std::uint8_t> bgra, std::size_t stride, std::uint64_t timestampMicroseconds, bool forceKeyFrame) {
    auto& state = *state_;
    if (stride < static_cast<std::size_t>(state.config.width) * 4 || stride > 64 * 1024 * 1024 / state.config.height || bgra.size() < stride * state.config.height) {
        return std::unexpected(VideoEncodeError::kInvalidPixels);
    }
    if (timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()) / 10 ||
        (state.lastTimestamp && timestampMicroseconds <= *state.lastTimestamp)) {
        return std::unexpected(VideoEncodeError::kInvalidTimestamp);
    }
    if (state.media) {
        if (!convertVideoPixels(bgra, stride, state.config.width, state.config.height, VideoPixelFormat::kNv12, state.nv12)) {
            return std::unexpected(VideoEncodeError::kInvalidPixels);
        }
        const bool keyRequired = forceKeyFrame || state.codec.empty() || state.baselinePending;
        const auto started = std::chrono::steady_clock::now();
        auto encoded = state.media->encode(state.nv12, timestampMicroseconds, keyRequired);
        if (state.withinLatencyBudget(started)) {
            if (auto result = state.acceptHardware(std::move(encoded), timestampMicroseconds, keyRequired)) {
                return std::move(*result);
            }
        }
        if (state.fallbackHardware(false)) {
            if (auto result = state.acceptHardware(state.media->encode(state.nv12, timestampMicroseconds, true), timestampMicroseconds, true)) {
                return std::move(*result);
            }
        }
        // Discard the failed codec's private output, never a transmitted delta.
        // Re-encode these same pixels as an independent software baseline.
        if (!state.software) {
            state.fallback();
        }
    }
    auto result = state.software->encode(bgra, stride, timestampMicroseconds, forceKeyFrame || state.baselinePending);
    if (result) {
        result->baselineReset = state.baselinePending;
        state.baselinePending = false;
        state.codec = result->codec;
        state.lastTimestamp = timestampMicroseconds;
    }
    return result;
}
} // namespace ultraterimal::agent
