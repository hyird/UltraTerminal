#include "software_video.transport.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

#include <wels/codec_api.h>

#include "avc.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"
#include "video_color.protocol.h"

namespace ultraterimal::agent {
namespace {
std::uint32_t codecBitrate(std::uint32_t bitrate, std::uint32_t framesPerSecond) {
    // OpenH264 caps its rate-control clock at 60 Hz. Preserve the requested
    // per-picture budget for callers above that cadence without buffering.
    bitrate = static_cast<std::uint32_t>(static_cast<std::uint64_t>(bitrate) * std::min(framesPerSecond, 60u) / framesPerSecond);
    // OpenH264 redistributes SPATIAL_LAYER_ALL through float even with
    // one layer. An unrepresentable integer can round UP past its exact layer
    // ceiling, causing SetOption(BITRATE) to fail after mutating codec state.
    // Multiples of 8 are exactly representable throughout our <=100 Mbps range.
    // Normalize target and ceilings together, down by at most 7 bits/second.
    return bitrate - bitrate % 8;
}
void requireCodec(int result, const char* operation) {
    if (result != cmResultSuccess) {
        throw std::runtime_error(operation);
    }
}
} // namespace
struct SoftwareVideoEncoder::State {
    ISVCEncoder* encoder = nullptr;
    bool initialized = false;
    bool refreshRequired = true;
    VideoEncoderConfig config;
    std::vector<std::uint8_t> i420;
    std::optional<std::uint64_t> lastTimestamp;
    std::string codec;
    ~State() {
        if (encoder) {
            if (initialized) {
                encoder->Uninitialize();
            }
            WelsDestroySVCEncoder(encoder);
        }
    }
};
SoftwareVideoEncoder::SoftwareVideoEncoder(VideoEncoderConfig config) : state_(std::make_unique<State>()) {
    const auto pixels = static_cast<std::uint64_t>(config.width) * config.height;
    if (config.width < 16 || config.height < 16 || config.width > 4096 || config.height > 4096 ||
        config.width % 2 || config.height % 2 || pixels > 4096 * 2160 ||
        config.bitrate < 64000 || config.bitrate > 100000000 || !config.framesPerSecond || config.framesPerSecond > 120) {
        throw std::invalid_argument("Invalid software video configuration");
    }
    const auto bitrate = codecBitrate(config.bitrate, config.framesPerSecond);
    state_->config = config;
    requireCodec(WelsCreateSVCEncoder(&state_->encoder), "Cannot create OpenH264 encoder");
    int traceLevel = WELS_LOG_QUIET;
    requireCodec(state_->encoder->SetOption(ENCODER_OPTION_TRACE_LEVEL, &traceLevel), "Cannot configure encoder logging");
    SEncParamExt parameters{};
    requireCodec(state_->encoder->GetDefaultParams(&parameters), "Cannot obtain encoder defaults");
    parameters.iMinQp = config.sharp ? 10 : 22;
    parameters.iMaxQp = config.sharp ? 18 : 51;
    parameters.iUsageType = CAMERA_VIDEO_REAL_TIME;
    parameters.iPicWidth = static_cast<int>(config.width);
    parameters.iPicHeight = static_cast<int>(config.height);
    parameters.iTargetBitrate = static_cast<int>(bitrate);
    parameters.iMaxBitrate = static_cast<int>(bitrate);
    parameters.iRCMode = RC_TIMESTAMP_MODE;
    parameters.fMaxFrameRate = static_cast<float>(std::min(config.framesPerSecond, 60u));
    parameters.iTemporalLayerNum = 1;
    parameters.iSpatialLayerNum = 1;
    parameters.iComplexityMode = LOW_COMPLEXITY;
    parameters.uiIntraPeriod = 0; // Reliable delivery; refresh and baseline changes force IDR.
    parameters.iNumRefFrame = 1;
    parameters.eSpsPpsIdStrategy = CONSTANT_ID;
    parameters.bPrefixNalAddingCtrl = false;
    parameters.bEnableSSEI = false;
    parameters.bEnableFrameSkip = false;
    parameters.bEnableLongTermReference = false;
    // CAMERA_VIDEO_REAL_TIME enables true parallel multi-slice encoding without
    // OpenH264's single-threaded screen-content full-frame hash search, while
    // background detection skips unchanged 16x16 desktop macroblocks via SIMD.
    const auto mbRows = (config.height + 15u) / 16u;
    const auto maximumThreads = std::min<unsigned>(16u, std::max(1u, mbRows / 4u));
    const auto threads = pixels >= 640 * 360 ? std::clamp(std::thread::hardware_concurrency(), 1u, maximumThreads) : 1u;
    parameters.iMultipleThreadIdc = static_cast<unsigned short>(threads);
    parameters.bUseLoadBalancing = false;
    parameters.iLoopFilterDisableIdc = threads > 1 ? 2 : 0;
    parameters.bEnableDenoise = false;
    parameters.bEnableBackgroundDetection = true;
    parameters.bEnableAdaptiveQuant = false;
    parameters.bEnableSceneChangeDetect = false;
    parameters.bFixRCOverShoot = true;
    parameters.bEnableFrameCroppingFlag = true;
    auto& layer = parameters.sSpatialLayers[0];
    layer.iVideoWidth = parameters.iPicWidth;
    layer.iVideoHeight = parameters.iPicHeight;
    layer.fFrameRate = parameters.fMaxFrameRate;
    layer.iSpatialBitrate = parameters.iTargetBitrate;
    layer.iMaxSpatialBitrate = parameters.iMaxBitrate;
    layer.uiProfileIdc = PRO_BASELINE;
    layer.sSliceArgument.uiSliceMode = threads > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE;
    layer.sSliceArgument.uiSliceNum = threads;
    layer.bVideoSignalTypePresent = true;
    layer.uiVideoFormat = VF_UNDEF;
    layer.bFullRange = false;
    layer.bColorDescriptionPresent = true;
    layer.uiColorPrimaries = CP_BT709;
    layer.uiTransferCharacteristics = TRC_IEC61966_2_1;
    layer.uiColorMatrix = CM_SMPTE170M;
    layer.bAspectRatioPresent = true;
    layer.eAspectRatio = ASP_1x1;
    requireCodec(state_->encoder->InitializeExt(&parameters), "Cannot initialize OpenH264 encoder");
    state_->initialized = true;
}
SoftwareVideoEncoder::~SoftwareVideoEncoder() = default;
void SoftwareVideoEncoder::setDetail(bool sharp) {
    if (state_->config.sharp == sharp) {
        return;
    }
    // A bitrate ceiling alone does not improve a static desktop's first IDR.
    // Bound quantization in detail mode; transport backpressure still bounds
    // outstanding frames when detailed content needs more bytes. OpenH264's
    // live parameter update does not rebuild its rate controller's QP limits.
    // Recreate only on a mode transition and start an independent IDR; keep
    // dimensions, codec profile and the external frame/generation sequence.
    auto config = state_->config;
    config.sharp = sharp;
    SoftwareVideoEncoder replacement(config);
    replacement.state_->lastTimestamp = state_->lastTimestamp;
    state_.swap(replacement.state_);
}
void SoftwareVideoEncoder::setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate) {
    if (!framesPerSecond || framesPerSecond > 120 || bitrate < 64000 || bitrate > 100000000) {
        throw std::invalid_argument("Invalid adaptive video rate");
    }
    const auto previousBitrate = codecBitrate(state_->config.bitrate, state_->config.framesPerSecond);
    const auto nextBitrate = codecBitrate(bitrate, framesPerSecond);
    if (framesPerSecond != state_->config.framesPerSecond) {
        float rate = static_cast<float>(std::min(framesPerSecond, 60u));
        requireCodec(state_->encoder->SetOption(ENCODER_OPTION_FRAME_RATE, &rate), "Cannot update encoder frame rate");
    }
    if (nextBitrate != previousBitrate) {
        SBitrateInfo layer{SPATIAL_LAYER_0, static_cast<int>(nextBitrate)};
        SBitrateInfo rate{SPATIAL_LAYER_ALL, static_cast<int>(nextBitrate)};
        const auto maximum = [&] {
            requireCodec(state_->encoder->SetOption(ENCODER_OPTION_MAX_BITRATE, &layer), "Cannot update layer maximum bitrate");
            requireCodec(state_->encoder->SetOption(ENCODER_OPTION_MAX_BITRATE, &rate), "Cannot update maximum bitrate");
        };
        const auto target = [&] {
            requireCodec(state_->encoder->SetOption(ENCODER_OPTION_BITRATE, &layer), "Cannot update layer bitrate");
            requireCodec(state_->encoder->SetOption(ENCODER_OPTION_BITRATE, &rate), "Cannot update encoder bitrate");
        };
        // Raise ceilings before the target; lower the target before ceilings.
        if (nextBitrate > previousBitrate) {
            maximum();
        }
        target();
        if (nextBitrate < previousBitrate) {
            maximum();
        }
    }
    state_->config.framesPerSecond = framesPerSecond;
    state_->config.bitrate = bitrate;
}
std::expected<EncodedVideoFrame, VideoEncodeError> SoftwareVideoEncoder::encodeI420(std::span<const std::uint8_t> i420,
                                                                                    std::uint64_t timestampMicroseconds,
                                                                                    bool forceKeyFrame) {
    auto& state = *state_;
    const auto width = state.config.width, height = state.config.height;
    const auto expectedBytes = static_cast<std::size_t>(width) * height * 3 / 2;
    if (i420.size() < expectedBytes) {
        return std::unexpected(VideoEncodeError::kInvalidPixels);
    }
    if (timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<long long>::max()) ||
        (state.lastTimestamp && timestampMicroseconds <= *state.lastTimestamp)) {
        return std::unexpected(VideoEncodeError::kInvalidTimestamp);
    }
    const bool requireKeyFrame = forceKeyFrame || state.refreshRequired;
    if (requireKeyFrame && state.encoder->ForceIntraFrame(true) != cmResultSuccess) {
        state.refreshRequired = true;
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    state.lastTimestamp = timestampMicroseconds;
    SSourcePicture picture{};
    picture.iColorFormat = videoFormatI420;
    picture.iPicWidth = static_cast<int>(width);
    picture.iPicHeight = static_cast<int>(height);
    picture.iStride[0] = static_cast<int>(width);
    picture.iStride[1] = picture.iStride[2] = static_cast<int>(width / 2);
    picture.pData[0] = const_cast<std::uint8_t*>(i420.data());
    picture.pData[1] = picture.pData[0] + static_cast<std::size_t>(width) * height;
    picture.pData[2] = picture.pData[1] + static_cast<std::size_t>(width) * height / 4;
    picture.uiTimeStamp = static_cast<long long>(timestampMicroseconds / 1000);
    // OpenH264 normalizes this millisecond timestamp for rate control. Keep
    // the caller's original microsecond timestamp on the wire, not its RC clock.
    SFrameBSInfo info{};
    state.refreshRequired = true; // Any failed or oversized output forces a new independent baseline.
    if (state.encoder->EncodeFrame(&picture, &info) != cmResultSuccess) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    if (info.eFrameType == videoFrameTypeSkip) {
        return std::unexpected(VideoEncodeError::kFrameSkipped);
    }
    constexpr auto kMaximumVideoBytes = protocol::kMaximumMessageBytes - 1024;
    if (info.iFrameSizeInBytes <= 0 || static_cast<std::size_t>(info.iFrameSizeInBytes) > kMaximumVideoBytes ||
        info.iLayerNum <= 0 || info.iLayerNum > MAX_LAYER_NUM_OF_FRAME) {
        return std::unexpected(VideoEncodeError::kFrameTooLarge);
    }
    EncodedVideoFrame frame;
    frame.timestampMicroseconds = timestampMicroseconds;
    frame.keyFrame = info.eFrameType == videoFrameTypeIDR;
    if (requireKeyFrame && !frame.keyFrame) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    frame.annexB.reserve(static_cast<std::size_t>(info.iFrameSizeInBytes));
    for (int i = 0; i < info.iLayerNum; ++i) {
        const auto& layer = info.sLayerInfo[i];
        if (layer.iNalCount < 0 || layer.iNalCount > MAX_NAL_UNITS_IN_LAYER || !layer.pBsBuf || !layer.pNalLengthInByte) {
            return std::unexpected(VideoEncodeError::kCodecFailure);
        }
        std::size_t offset = 0;
        for (int j = 0; j < layer.iNalCount; ++j) {
            const auto length = layer.pNalLengthInByte[j];
            if (length <= 0 || frame.annexB.size() + static_cast<std::size_t>(length) > static_cast<std::size_t>(info.iFrameSizeInBytes)) {
                return std::unexpected(VideoEncodeError::kFrameTooLarge);
            }
            const std::span<const std::uint8_t> nal(layer.pBsBuf + offset, static_cast<std::size_t>(length));
            frame.annexB.insert(frame.annexB.end(), nal.begin(), nal.end());
            offset += static_cast<std::size_t>(length);
        }
    }
    const auto description = describeAvcFrame(frame.annexB, state.codec);
    if (frame.annexB.size() != static_cast<std::size_t>(info.iFrameSizeInBytes) || !description || description->keyFrame != frame.keyFrame) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    state.codec = description->codec;
    frame.codec = state.codec;
    state.refreshRequired = false;
    return frame;
}
std::expected<EncodedVideoFrame, VideoEncodeError> SoftwareVideoEncoder::encode(std::span<const std::uint8_t> bgra,
                                                                                std::size_t stride,
                                                                                std::uint64_t timestampMicroseconds,
                                                                                bool forceKeyFrame) {
    auto& state = *state_;
    const auto width = state.config.width, height = state.config.height;
    if (stride < static_cast<std::size_t>(width) * 4 || stride > 64 * 1024 * 1024 / height || bgra.size() < stride * height) {
        return std::unexpected(VideoEncodeError::kInvalidPixels);
    }
    if (timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        (state.lastTimestamp && timestampMicroseconds <= *state.lastTimestamp)) {
        return std::unexpected(VideoEncodeError::kInvalidTimestamp);
    }
    state.i420.resize(static_cast<std::size_t>(width) * height * 3 / 2);
    if (!convertVideoPixels(bgra, stride, width, height, VideoPixelFormat::kI420, state.i420)) {
        return std::unexpected(VideoEncodeError::kInvalidPixels);
    }
    return encodeI420(state.i420, timestampMicroseconds, forceKeyFrame);
}
} // namespace ultraterimal::agent
