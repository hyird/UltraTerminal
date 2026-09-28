#include "video.transport.h"

#include <limits>
#include <optional>
#include <stdexcept>

#include <wels/codec_api.h>

#include "avc.protocol.h"
#include "platform/windows/media_encoder.transport.h"
#include "software_video.transport.h"
#include "video_color.protocol.h"

namespace ultraterimal::agent {
namespace {
bool verifyHardwareBaseline(const MediaEncodedFrame& frame, const VideoEncoderConfig& config) {
    ISVCDecoder* raw = nullptr;
    if (WelsCreateDecoder(&raw) != 0 || !raw) {
        return false;
    }
    struct Decoder {
        ISVCDecoder* value;
        ~Decoder() {
            value->Uninitialize();
            WelsDestroyDecoder(value);
        }
    } decoder{raw};
    SDecodingParam settings{};
    settings.eEcActiveIdc = ERROR_CON_DISABLE;
    settings.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (raw->Initialize(&settings) != 0) {
        return false;
    }
    unsigned char* planes[3]{};
    SBufferInfo output{};
    const auto status = raw->DecodeFrameNoDelay(frame.bytes.data(), static_cast<int>(frame.bytes.size()), planes, &output);
    return status == dsErrorFree && output.iBufferStatus == 1 &&
           output.UsrData.sSystemBuffer.iWidth == static_cast<int>(config.width) &&
           output.UsrData.sSystemBuffer.iHeight == static_cast<int>(config.height);
}
} // namespace
struct VideoEncoder::State {
    VideoEncoderConfig config;
    std::unique_ptr<MediaEncoder> media;
    std::unique_ptr<SoftwareVideoEncoder> software;
    std::vector<std::uint8_t> nv12;
    std::optional<std::uint64_t> lastTimestamp;
    std::string codec;
    bool baselinePending = false;
    bool hardwareValidated = false;

    void fallback() {
        media.reset();
        std::vector<std::uint8_t>().swap(nv12);
        software = std::make_unique<SoftwareVideoEncoder>(config);
        baselinePending = !codec.empty();
    }
};
VideoEncoder::VideoEncoder(VideoEncoderConfig config) : state_(std::make_unique<State>()) {
    const auto pixels = static_cast<std::uint64_t>(config.width) * config.height;
    if (config.width < 16 || config.height < 16 || config.width > 4096 || config.height > 4096 ||
        config.width % 2 || config.height % 2 || pixels > 4096 * 2160 || !config.framesPerSecond || config.framesPerSecond > 120 ||
        config.bitrate < 64000 || config.bitrate > 100000000) {
        throw std::invalid_argument("Invalid video configuration");
    }
    state_->config = config;
    try {
        state_->media = std::make_unique<MediaEncoder>(MediaEncoderConfig{config.width, config.height, config.bitrate, config.framesPerSecond, config.sharp});
        state_->nv12.resize(static_cast<std::size_t>(pixels * 3 / 2));
    } catch (const std::runtime_error&) {
        state_->fallback();
    }
}
VideoEncoder::~VideoEncoder() = default;
bool VideoEncoder::hardware() const { return state_->media != nullptr; }
void VideoEncoder::setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate) {
    if (!framesPerSecond || framesPerSecond > 120 || bitrate < 64000 || bitrate > 100000000) {
        throw std::invalid_argument("Invalid adaptive video rate");
    }
    state_->config.framesPerSecond = framesPerSecond;
    state_->config.bitrate = bitrate;
    if (state_->media && !state_->media->setRate(framesPerSecond, bitrate)) {
        state_->fallback();
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
        state_->fallback();
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
        const bool keyRequired = forceKeyFrame || state.codec.empty();
        auto result = state.media->encode(state.nv12, timestampMicroseconds, keyRequired);
        if (result && result->timestampMicroseconds == timestampMicroseconds) {
            auto description = describeAvcFrame(result->bytes, state.codec);
            const bool needsValidation = !state.hardwareValidated || (description && state.codec != description->codec);
            if (description && description->keyFrame == result->keyFrame && (!keyRequired || description->keyFrame) &&
                (!needsValidation || (description->keyFrame && verifyHardwareBaseline(*result, state.config)))) {
                state.hardwareValidated = true;
                const bool changed = !state.codec.empty() && state.codec != description->codec;
                state.codec = description->codec;
                state.lastTimestamp = timestampMicroseconds;
                return EncodedVideoFrame{std::move(result->bytes), state.codec, timestampMicroseconds, description->keyFrame, changed};
            }
        }
        // Discard the failed codec's private output, never a transmitted delta.
        // Re-encode these same pixels as an independent software baseline.
        state.fallback();
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
