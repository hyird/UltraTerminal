#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <wels/codec_api.h>

#include "features/desktop/software_video.transport.h"
#include "features/desktop/video.transport.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace {
using namespace ultraterimal::agent;
class Decoder {
  public:
    Decoder() {
        if (WelsCreateDecoder(&decoder_) != 0 || !decoder_) {
            throw std::runtime_error("Cannot create test decoder");
        }
        int quiet = WELS_LOG_QUIET;
        decoder_->SetOption(DECODER_OPTION_TRACE_LEVEL, &quiet);
        SDecodingParam config{};
        config.eEcActiveIdc = ERROR_CON_DISABLE;
        config.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if (decoder_->Initialize(&config) != 0) {
            WelsDestroyDecoder(decoder_);
            decoder_ = nullptr;
            throw std::runtime_error("Cannot initialize test decoder");
        }
    }
    ~Decoder() {
        decoder_->Uninitialize();
        WelsDestroyDecoder(decoder_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    double lumaError(const EncodedVideoFrame& frame, std::span<const std::uint8_t> source, unsigned width, unsigned height) {
        unsigned char* planes[3]{};
        SBufferInfo info{};
        if (decoder_->DecodeFrameNoDelay(frame.annexB.data(), static_cast<int>(frame.annexB.size()), planes, &info) != dsErrorFree || info.iBufferStatus != 1) {
            throw std::runtime_error("Detail fixture failed to decode");
        }
        double error = 0;
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const int expected = ((220 * source[(y * width + x) * 4] + 128) >> 8) + 16;
                const int delta = planes[0][y * info.UsrData.sSystemBuffer.iStride[0] + x] - expected;
                error += delta * delta;
            }
        }
        return error / (width * height);
    }
    void verify(const EncodedVideoFrame& frame, int width, int height, bool changed) {
        unsigned char* planes[3]{};
        SBufferInfo info{};
        info.uiInBsTimeStamp = frame.timestampMicroseconds;
        const auto status = decoder_->DecodeFrameNoDelay(frame.annexB.data(), static_cast<int>(frame.annexB.size()), planes, &info);
        if (status != dsErrorFree || info.iBufferStatus != 1 || info.UsrData.sSystemBuffer.iWidth != width ||
            info.UsrData.sSystemBuffer.iHeight != height || info.uiOutYuvTimeStamp != frame.timestampMicroseconds) {
            std::cerr << "Decode status=" << status << " ready=" << info.iBufferStatus << " size=" << info.UsrData.sSystemBuffer.iWidth << "x" << info.UsrData.sSystemBuffer.iHeight << " expected=" << width << "x" << height << " timestamp=" << info.uiOutYuvTimeStamp << " expected=" << frame.timestampMicroseconds << '\n';
            throw std::runtime_error("Encoded frame did not decode immediately at the expected dimensions/timestamp");
        }
        // Independent known limited-range color values check RGB order, range, stride and chroma planes.
        const std::array<std::array<int, 3>, 6> expected{{{16, 128, 128}, {235, 128, 128}, {82, 90, 240}, {144, 54, 34}, changed ? std::array<int, 3>{126, 128, 128} : std::array<int, 3>{41, 240, 110}, {126, 128, 128}}};
        for (int bar = 0; bar < 6; ++bar) {
            const int x = (bar * 2 + 1) * width / 12, y = height / 2;
            const auto& buffer = info.UsrData.sSystemBuffer;
            const std::array<int, 3> samples{planes[0][y * buffer.iStride[0] + x],
                                             planes[1][(y / 2) * buffer.iStride[1] + x / 2],
                                             planes[2][(y / 2) * buffer.iStride[1] + x / 2]};
            for (std::size_t plane = 0; plane < 3; ++plane) {
                if (std::abs(samples[plane] - expected[static_cast<std::size_t>(bar)][plane]) > 12) {
                    throw std::runtime_error("Decoded color bars differ from expected color conversion");
                }
            }
        }
    }

  private:
    ISVCDecoder* decoder_ = nullptr;
};
std::vector<std::uint8_t> pixels(std::uint32_t width, std::uint32_t height, std::size_t stride, bool changed) {
    const std::array<std::array<std::uint8_t, 3>, 6> colors{{{0, 0, 0}, {255, 255, 255}, {0, 0, 255}, {0, 255, 0}, changed ? std::array<std::uint8_t, 3>{128, 128, 128} : std::array<std::uint8_t, 3>{255, 0, 0}, {128, 128, 128}}};
    std::vector<std::uint8_t> image(stride * height, 0xcd);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                image[y * stride + x * 4 + c] = colors[x * 6 / width][c];
            }
            image[y * stride + x * 4 + 3] = 255;
        }
    }
    return image;
}
template <typename Encoder = SoftwareVideoEncoder>
void verifyResolution(std::uint32_t width, std::uint32_t height, std::ofstream* manifest, const std::filesystem::path& directory) {
    Encoder encoder({width, height, 8000000, 30});
    Decoder decoder;
    ultraterimal::protocol::DesktopChannelValidator channel({"session", "video", ultraterimal::wire::DESKTOP_VIDEO, ultraterimal::protocol::ChannelSource::kAgent, false});
    const auto stride = static_cast<std::size_t>(width) * 4 + 64;
    for (unsigned i = 0; i < 4; ++i) {
        encoder.setRate(i % 2 ? 120 : 20, i % 2 ? 8000000 : 4000000);
        encoder.setDetail(i % 2 != 0);
        const auto image = pixels(width, height, stride, i != 0);
        const auto encoded = encoder.encode(image, stride, i * 33333, i == 3);
        if (!encoded || encoded->annexB.empty() || !encoded->codec.starts_with("avc1.42") ||
            ((i == 0 || i == 3) && !encoded->keyFrame)) {
            throw std::runtime_error("Missing valid AVC output or requested keyframe");
        }
        decoder.verify(*encoded, static_cast<int>(width), static_cast<int>(height), i != 0);
        if constexpr (requires { encoder.hardware(); }) {
            std::cout << "Automatic " << width << 'x' << height << " frame=" << i << " hardware=" << encoder.hardware() << " codec=" << encoded->codec << '\n';
        }
        ultraterimal::wire::Envelope message;
        message.set_protocol_version(ultraterimal::protocol::kVersion);
        message.set_session_id("session");
        message.set_channel_id("video");
        message.set_generation(1);
        if (i == 0) {
            message.set_sequence(2);
            message.mutable_video_config()->set_codec(encoded->codec);
            message.mutable_video_config()->set_width(width);
            message.mutable_video_config()->set_height(height);
            if (!channel.accept(message)) {
                throw std::runtime_error("Encoder configuration violates desktop channel contract");
            }
        }
        message.set_sequence(i + 3);
        message.set_frame_id(i + 1);
        message.mutable_video_frame()->set_data(reinterpret_cast<const char*>(encoded->annexB.data()), encoded->annexB.size());
        message.mutable_video_frame()->set_key_frame(encoded->keyFrame);
        message.mutable_video_frame()->set_timestamp_microseconds(encoded->timestampMicroseconds);
        if (!channel.accept(message)) {
            throw std::runtime_error("Encoded video violates desktop channel contract");
        }
        if (manifest) {
            const auto name = std::to_string(width) + "-" + std::to_string(height) + "-" + std::to_string(i) + ".h264";
            std::ofstream output(directory / name, std::ios::binary);
            output.write(reinterpret_cast<const char*>(encoded->annexB.data()), static_cast<std::streamsize>(encoded->annexB.size()));
            if (!output) {
                throw std::runtime_error("Cannot write synthetic video fixture");
            }
            if (width != 320 || i != 0) {
                *manifest << ",";
            }
            *manifest << "{\"file\":\"" << name << "\",\"width\":" << width << ",\"height\":" << height
                      << ",\"codec\":\"" << encoded->codec << "\",\"timestamp\":" << encoded->timestampMicroseconds
                      << ",\"key\":" << (encoded->keyFrame ? "true" : "false") << ",\"changed\":" << (i ? "true" : "false") << "}";
        }

        if (i == 3) {
            Decoder fresh;
            fresh.verify(*encoded, static_cast<int>(width), static_cast<int>(height), true);
        }
    }
    const auto image = pixels(width, height, stride, false);
    const auto duplicate = encoder.encode(image, stride, 99999);
    if (duplicate || duplicate.error() != VideoEncodeError::kInvalidTimestamp) {
        throw std::runtime_error("Duplicate timestamp accepted");
    }
    const auto truncated = encoder.encode(std::span(image).first(16), stride, 200000);
    if (truncated || truncated.error() != VideoEncodeError::kInvalidPixels) {
        throw std::runtime_error("Truncated pixels accepted");
    }
    const auto overflow = encoder.encode(image, std::numeric_limits<std::size_t>::max(), 200000);
    if (overflow || overflow.error() != VideoEncodeError::kInvalidPixels) {
        throw std::runtime_error("Overflowing stride accepted");
    }
    const auto recovered = encoder.encode(image, stride, 200000, true);
    if (!recovered || !recovered->keyFrame) {
        throw std::runtime_error("Encoder did not recover after rejected input");
    }
    Decoder fresh;
    fresh.verify(*recovered, static_cast<int>(width), static_cast<int>(height), false);
    std::cout << "Verified " << width << "x" << height << " " << recovered->codec << '\n';
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        // Dense one-pixel strokes exercise static text-like detail, independently
        // decoded at identical bitrate. This catches a cosmetic bitrate-only mode.
        constexpr unsigned width = 1280, height = 720;
        std::vector<std::uint8_t> strokes(width * height * 4, 255);
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto value = static_cast<std::uint8_t>((x % 11 == 2 || (y % 13 == 3 && x % 11 < 7)) ? 32 : 240);
                for (unsigned c = 0; c < 3; ++c) {
                    strokes[(y * width + x) * 4 + c] = value;
                }
            }
        }
        std::array<double, 2> errors{};
        for (unsigned sharp = 0; sharp < 2; ++sharp) {
            SoftwareVideoEncoder encoder({width, height, 8000000, 60});
            encoder.setDetail(sharp != 0);
            auto frame = encoder.encode(strokes, width * 4, 0, true);
            if (!frame || !frame->keyFrame) {
                throw std::runtime_error("Missing detail fixture IDR");
            }
            Decoder decoder;
            errors[sharp] = decoder.lumaError(*frame, strokes, width, height);
        }
        std::cout << "Static stroke MSE balanced=" << errors[0] << " sharp=" << errors[1] << '\n';
        if (errors[1] >= errors[0] * 0.8) {
            throw std::runtime_error("Sharp mode did not materially improve static detail");
        }
        std::filesystem::path directory;
        std::ofstream manifest;
        const bool automatic = argc == 3 && std::wstring_view(argv[1]) == L"--automatic-fixtures";
        if (argc == 3 && (automatic || std::wstring_view(argv[1]) == L"--fixtures")) {
            directory = argv[2];
            std::filesystem::create_directories(directory);
            manifest.open(directory / "manifest.json");
            if (!manifest) {
                throw std::runtime_error("Cannot create fixture manifest");
            }
            manifest << "[";
        } else if (argc != 1) {
            return 2;
        }

        for (const auto invalid : {VideoEncoderConfig{}, VideoEncoderConfig{321, 180}, VideoEncoderConfig{320, 181}, VideoEncoderConfig{4096, 4096}}) {
            bool rejected = false;
            try {
                SoftwareVideoEncoder encoder(invalid);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("Invalid encoder configuration accepted");
            }
        }
        if (automatic) {
            verifyResolution<ultraterimal::agent::VideoEncoder>(320, 180, &manifest, directory);
            verifyResolution<ultraterimal::agent::VideoEncoder>(2560, 1600, &manifest, directory);
        } else {
            verifyResolution(320, 180, directory.empty() ? nullptr : &manifest, directory);
            verifyResolution(2560, 1600, directory.empty() ? nullptr : &manifest, directory);
        }
        if (!directory.empty()) {
            manifest << "]";
            if (!manifest) {
                throw std::runtime_error("Cannot finish fixture manifest");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
