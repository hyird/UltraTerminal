#include "software_video.transport.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <x264.h>

#include "avc.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"
#include "video_color.protocol.h"

namespace ultraterimal::agent {
namespace {
int codecBitrate(std::uint32_t bitrate, std::uint32_t nominalFps, std::uint32_t captureFps) {
    // x264 cannot reconfigure FPS; VFR introduces one-frame lookahead. Keep
    // its initial CFR clock and scale the per-frame budget instead. Units are
    // integer kbit/s with x264's supported limits; transport pacing is separate.
    return static_cast<int>(std::clamp<std::uint64_t>(static_cast<std::uint64_t>(bitrate) * nominalFps / captureFps / 1000, 1, 2000000));
}
} // namespace
struct SoftwareVideoEncoder::State {
    x264_t* encoder = nullptr;
    bool refreshRequired = true;
    VideoEncoderConfig config;
    std::uint32_t nominalFps = 0;
    std::uint64_t nextPicture = 0;
    std::vector<std::uint8_t> i420;
    std::optional<std::uint64_t> lastTimestamp;
    std::string codec;
    ~State() {
        if (encoder) {
            x264_encoder_close(encoder);
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
    state_->config = config;
    state_->nominalFps = config.framesPerSecond;
    state_->i420.resize(static_cast<std::size_t>(pixels * 3 / 2));
    x264_param_t parameters{};
    if (x264_param_default_preset(&parameters, "superfast", "zerolatency,psnr") < 0) {
        throw std::runtime_error("Cannot configure x264 preset");
    }
    parameters.i_width = static_cast<int>(config.width);
    parameters.i_height = static_cast<int>(config.height);
    parameters.i_csp = X264_CSP_I420;
    parameters.i_bitdepth = 8;
    // Slice threading has no frame queue. Leave CPU capacity for capture/input.
    const auto maximumThreads = pixels <= 1920 * 1080 ? 2u : 4u;
    const auto threads = pixels >= 1280 * 720 ? std::clamp(std::thread::hardware_concurrency() / 2, 1u, maximumThreads) : 1u;
    parameters.i_threads = static_cast<int>(threads);
    parameters.b_sliced_threads = 1;
    parameters.i_slice_count = static_cast<int>(threads);
    parameters.i_frame_reference = 1;
    // Reliable ordered transport preserves references. Periodic IDRs can be
    // much larger than the ACK byte window and interrupt an otherwise steady
    // cadence; baseline changes and explicit recovery still force an IDR.
    parameters.i_keyint_max = X264_KEYINT_MAX_INFINITE;
    parameters.i_scenecut_threshold = 0;
    parameters.i_bframe = 0;
    parameters.i_fps_num = config.framesPerSecond;
    parameters.i_fps_den = 1;
    parameters.i_timebase_num = 1;
    parameters.i_timebase_den = config.framesPerSecond;
    // VFR adds one buffered frame even with zerolatency. Use a private CFR
    // sequence; the wire frame keeps the original capture timestamp.
    parameters.b_vfr_input = 0;
    parameters.b_annexb = 1;
    parameters.b_repeat_headers = 1;
    parameters.i_log_level = X264_LOG_NONE;
    parameters.rc.i_rc_method = X264_RC_ABR;
    parameters.rc.i_bitrate = codecBitrate(config.bitrate, state_->nominalFps, config.framesPerSecond);
    parameters.rc.i_vbv_max_bitrate = parameters.rc.i_bitrate;
    parameters.rc.i_vbv_buffer_size = parameters.rc.i_bitrate;
    parameters.rc.i_qp_min = config.sharp ? 10 : 0;
    parameters.rc.i_qp_max = config.sharp ? 18 : 51;
    // Sharp mode may exceed VBV when the quality floor cannot fit the budget;
    // the existing bounded frame/ACK pacing remains authoritative for transport.
    parameters.vui.i_vidformat = 5;
    parameters.vui.b_fullrange = 0;
    parameters.vui.i_colorprim = 1; // BT.709 primaries.
    parameters.vui.i_transfer = 13; // IEC 61966-2-1 transfer.
    parameters.vui.i_colmatrix = 6; // SMPTE 170M / BT.601 limited-range matrix.
    parameters.vui.i_sar_width = 1;
    parameters.vui.i_sar_height = 1;
    if (x264_param_apply_profile(&parameters, "baseline") < 0) {
        throw std::runtime_error("Cannot configure x264 Baseline profile");
    }
    state_->encoder = x264_encoder_open(&parameters);
    if (!state_->encoder) {
        throw std::runtime_error("Cannot initialize x264 encoder");
    }
    x264_encoder_parameters(state_->encoder, &parameters);
    if (parameters.i_bframe || parameters.rc.i_lookahead || parameters.i_sync_lookahead || (parameters.i_threads > 1 && !parameters.b_sliced_threads) ||
        x264_encoder_maximum_delayed_frames(state_->encoder) != 0) {
        throw std::runtime_error("x264 changed the low-latency timestamp contract");
    }
}
SoftwareVideoEncoder::~SoftwareVideoEncoder() = default;
void SoftwareVideoEncoder::setDetail(bool sharp) {
    if (state_->config.sharp == sharp) {
        return;
    }
    // QP bounds cannot be reconfigured through x264_encoder_reconfig. Rebuild
    // only on quality-mode changes, preserving timestamp order and emitting IDR.
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
    if (bitrate != state_->config.bitrate || framesPerSecond != state_->config.framesPerSecond) {
        x264_param_t parameters{};
        x264_encoder_parameters(state_->encoder, &parameters);
        parameters.rc.i_bitrate = codecBitrate(bitrate, state_->nominalFps, framesPerSecond);
        parameters.rc.i_vbv_max_bitrate = parameters.rc.i_bitrate;
        parameters.rc.i_vbv_buffer_size = static_cast<int>(bitrate / 1000);
        if (x264_encoder_reconfig(state_->encoder, &parameters) < 0) {
            throw std::runtime_error("Cannot update x264 bitrate");
        }
    }
    state_->config.framesPerSecond = framesPerSecond;
    state_->config.bitrate = bitrate;
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
    if (!convertVideoPixels(bgra, stride, width, height, VideoPixelFormat::kI420, state.i420)) {
        return std::unexpected(VideoEncodeError::kInvalidPixels);
    }
    const bool requireKeyFrame = forceKeyFrame || state.refreshRequired;
    x264_picture_t picture{}, output{};
    x264_picture_init(&picture);
    picture.img.i_csp = X264_CSP_I420;
    picture.img.i_plane = 3;
    picture.img.i_stride[0] = static_cast<int>(width);
    picture.img.i_stride[1] = picture.img.i_stride[2] = static_cast<int>(width / 2);
    picture.img.plane[0] = state.i420.data();
    picture.img.plane[1] = picture.img.plane[0] + static_cast<std::size_t>(width) * height;
    picture.img.plane[2] = picture.img.plane[1] + static_cast<std::size_t>(width) * height / 4;
    picture.i_pts = static_cast<std::int64_t>(state.nextPicture++);
    picture.i_type = requireKeyFrame ? X264_TYPE_IDR : X264_TYPE_AUTO;
    state.lastTimestamp = timestampMicroseconds;
    state.refreshRequired = true;
    x264_nal_t* nals = nullptr;
    int count = 0;
    const int bytes = x264_encoder_encode(state.encoder, &nals, &count, &picture, &output);
    if (bytes < 0 || x264_encoder_delayed_frames(state.encoder) != 0) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    if (!bytes) {
        return std::unexpected(VideoEncodeError::kFrameSkipped);
    }
    constexpr auto kMaximumVideoBytes = protocol::kMaximumMessageBytes - 1024;
    if (static_cast<std::size_t>(bytes) > kMaximumVideoBytes) {
        return std::unexpected(VideoEncodeError::kFrameTooLarge);
    }
    if (!nals || count <= 0 || count > bytes || output.i_pts != picture.i_pts) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    EncodedVideoFrame frame;
    frame.timestampMicroseconds = timestampMicroseconds;
    frame.keyFrame = output.i_type == X264_TYPE_IDR;
    if (requireKeyFrame && !frame.keyFrame) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    frame.annexB.reserve(static_cast<std::size_t>(bytes));
    for (int i = 0; i < count; ++i) {
        const auto length = nals[i].i_payload;
        if (!nals[i].p_payload || length <= 0 || static_cast<std::size_t>(length) > static_cast<std::size_t>(bytes) - frame.annexB.size()) {
            return std::unexpected(VideoEncodeError::kCodecFailure);
        }
        const std::span<const std::uint8_t> nal(nals[i].p_payload, static_cast<std::size_t>(length));
        frame.annexB.insert(frame.annexB.end(), nal.begin(), nal.end());
    }
    const auto description = describeAvcFrame(frame.annexB, state.codec);
    if (frame.annexB.size() != static_cast<std::size_t>(bytes) || !description || description->keyFrame != frame.keyFrame) {
        return std::unexpected(VideoEncodeError::kCodecFailure);
    }
    state.codec = description->codec;
    frame.codec = state.codec;
    state.refreshRequired = false;
    return frame;
}
} // namespace ultraterimal::agent
