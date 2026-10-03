#include <iostream>
#include <stdexcept>

#include <wels/codec_api.h>

#include "features/desktop/avc.protocol.h"
#include "features/desktop/codecs.protocol.h"
#include "features/desktop/software_video.transport.h"
#include "features/desktop/video.transport.h"
#include "platform/windows/media_encoder.transport.h"

namespace {
enum class Fault { kNone,
                   kUnavailable,
                   kEncode,
                   kRate,
                   kDetail,
                   kMalformed,
                   kUndecodableKey,
                   kTimestamp,
                   kFalseKey };
Fault fault = Fault::kNone;
unsigned creations = 0, destructions = 0;
std::vector<ultraterimal::wire::VideoCodec> attemptedCodecs;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void decode(const ultraterimal::agent::EncodedVideoFrame& frame) {
    ISVCDecoder* raw = nullptr;
    require(WelsCreateDecoder(&raw) == 0 && raw, "Create decoder");
    struct Owner {
        ISVCDecoder* value;
        ~Owner() {
            value->Uninitialize();
            WelsDestroyDecoder(value);
        }
    } owner{raw};
    SDecodingParam config{};
    config.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    config.eEcActiveIdc = ERROR_CON_DISABLE;
    require(raw->Initialize(&config) == 0, "Initialize decoder");
    unsigned char* planes[3]{};
    SBufferInfo info{};
    require(raw->DecodeFrameNoDelay(frame.annexB.data(), static_cast<int>(frame.annexB.size()), planes, &info) == dsErrorFree && info.iBufferStatus == 1,
            "Fallback is not independently decodable");
    require(info.UsrData.sSystemBuffer.iWidth == 64 && info.UsrData.sSystemBuffer.iHeight == 64 && planes[0][0] > 85 && planes[0][0] < 105,
            "Fallback lost current pixels or dimensions");
}
} // namespace

// Link-time platform double: production selector and software encoder remain
// unchanged. No test-only flags or injectable failures enter the shipping Agent.
namespace ultraterimal::agent {
struct MediaEncoder::State {
    SoftwareVideoEncoder encoder;
    std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(64 * 64 * 4, 90);
    explicit State(MediaEncoderConfig config) : encoder({config.width, config.height, config.bitrate, config.framesPerSecond}) {}
};
MediaEncoder::MediaEncoder(MediaEncoderConfig config, ID3D11Device*) {
    ++creations;
    attemptedCodecs.push_back(config.codec);
    if (fault == Fault::kUnavailable) {
        throw std::runtime_error("Optional media unavailable");
    }
    state_ = std::make_unique<State>(config);
}
MediaEncoder::~MediaEncoder() { ++destructions; }
bool MediaEncoder::gpu() const { return false; }
std::expected<MediaEncodedFrame, HRESULT> MediaEncoder::encodeTexture(ID3D11Texture2D*, std::uint64_t, bool) { return std::unexpected(E_NOTIMPL); }
std::expected<void, HRESULT> MediaEncoder::setRate(std::uint32_t, std::uint32_t) {
    return fault == Fault::kRate ? std::expected<void, HRESULT>(std::unexpected(E_FAIL)) : std::expected<void, HRESULT>();
}
std::expected<void, HRESULT> MediaEncoder::setDetail(bool) {
    return fault == Fault::kDetail ? std::expected<void, HRESULT>(std::unexpected(E_FAIL)) : std::expected<void, HRESULT>();
}
std::expected<MediaEncodedFrame, HRESULT> MediaEncoder::encode(std::span<const std::uint8_t>, std::uint64_t timestamp, bool key) {
    if (fault == Fault::kEncode) {
        return std::unexpected(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    auto frame = state_->encoder.encode(state_->pixels, 64 * 4, timestamp, key);
    require(bool(frame), "Platform double encode");
    if (fault == Fault::kMalformed) {
        frame->annexB = {0, 0, 1, 7};
    }
    if (fault == Fault::kUndecodableKey) {
        for (std::size_t at = 2; at + 2 < frame->annexB.size(); ++at) {
            if (frame->annexB[at - 2] == 0 && frame->annexB[at - 1] == 0 && frame->annexB[at] == 1 && (frame->annexB[at + 1] & 31) == 5) {
                frame->annexB.resize(at + 3);
                frame->annexB.back() = 0xff;
                break;
            }
        }
        require(bool(describeAvcFrame(frame->annexB, frame->codec)), "Undecodable refresh must pass structural validation");
    }
    return MediaEncodedFrame{std::move(frame->annexB), fault == Fault::kTimestamp ? timestamp + 1 : timestamp, fault == Fault::kFalseKey ? !frame->keyFrame : frame->keyFrame};
}
} // namespace ultraterimal::agent

int main() {
    using namespace ultraterimal::agent;
    try {
        std::vector<std::uint8_t> pixels(64 * 64 * 4, 90);
        for (auto failure : {Fault::kUnavailable, Fault::kEncode, Fault::kRate, Fault::kDetail, Fault::kMalformed, Fault::kUndecodableKey, Fault::kTimestamp, Fault::kFalseKey}) {
            fault = failure == Fault::kUnavailable ? failure : Fault::kNone;
            const auto before = creations;
            VideoEncoder encoder({64, 64, 1000000, 60});
            auto first = encoder.encode(pixels, 256, 1, true);
            require(first && first->keyFrame && !first->baselineReset, "Initial baseline invalid");
            decode(*first);
            require(!encoder.encode({}, 256, 2) && !encoder.encode(pixels, 256, 1), "Invalid input accepted");
            fault = failure;
            if (failure != Fault::kUndecodableKey) {
                encoder.setRate(30, 2000000);
                encoder.setDetail(true);
            }
            auto fallback = encoder.encode(pixels, 256, 2, true);
            require(fallback && fallback->keyFrame && fallback->timestampMicroseconds == 2 && !encoder.hardware(), "Hardware failure did not fall back");
            require(fallback->baselineReset == (failure != Fault::kUnavailable), "Failover did not announce exactly one new baseline");
            decode(*fallback);
            fault = Fault::kNone;
            for (std::uint64_t timestamp = 3; timestamp < 20; ++timestamp) {
                const auto next = encoder.encode(pixels, 256, timestamp);
                require(next && !next->baselineReset && !encoder.hardware(), "Failed hardware retried or baseline repeatedly reset");
            }
            require(creations == before + 1, "Unbounded hardware retry");
        }
        require(destructions == 7, "Hardware codec was not released on failure");
        fault = Fault::kNone;
        attemptedCodecs.clear();
        {
            // The hardware double emits AVC even when asked for AV1. It must
            // never be labeled or transmitted as the requested family.
            VideoEncoder encoder({64, 64, 1000000, 60, false, {ultraterimal::wire::AVC, ultraterimal::wire::HEVC, ultraterimal::wire::AV1}});
            auto baseline = encoder.encode(pixels, 256, 1, true);
            require(baseline && baseline->codec.starts_with("avc1."), "Malformed optional codec failed to return H264");
            decode(*baseline);
            require(attemptedCodecs == std::vector{ultraterimal::wire::AV1, ultraterimal::wire::AVC}, "Optional codec retry was unbounded");
            for (std::uint64_t timestamp = 2; timestamp < 10; ++timestamp) {
                require(encoder.encode(pixels, 256, timestamp).has_value(), "H264 after optional fallback failed");
            }
            require(attemptedCodecs.size() == 2, "Rejected optional codec was retried");
        }
        for (const auto family : {ultraterimal::wire::HEVC, ultraterimal::wire::AV1}) {
            const std::vector<std::uint8_t> malformed{0, 0, 1, 0x65, 1};
            require(!describeVideoFrame(family, malformed, "", true), "Malformed optional frame accepted");
            require(!verifyVideoBaseline(family, malformed, 64, 64), "Malformed optional baseline decoded");
        }
        const std::vector<std::vector<std::uint8_t>> malformed{{}, {0, 0, 1}, {0, 0, 1, 0x87, 66, 0, 30}, {0, 0, 1, 0x65, 1}, {0, 0, 1, 0x67, 66}, {0, 0, 1, 0x67, 66, 0, 30, 0, 0, 1, 0x68, 1}};
        for (const auto& bytes : malformed) {
            require(!describeAvcFrame(bytes, "avc1.42001E"), "Malformed or incomplete AVC accepted");
        }
        std::cout << "PASS: hardware absence, timeout, rate/detail failures, malformed output, timestamp/key mismatch, baseline reset, decoding and bounded retries\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
