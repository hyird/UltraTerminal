#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <wels/codec_api.h>

#include "features/desktop/software_video.transport.h"

namespace {
using namespace ultraterimal::agent;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
class Decoder {
  public:
    Decoder() {
        require(WelsCreateDecoder(&value_) == 0 && value_, "Cannot create regression decoder");
        SDecodingParam config{};
        config.eEcActiveIdc = ERROR_CON_DISABLE;
        config.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if (value_->Initialize(&config) != 0) {
            WelsDestroyDecoder(value_);
            throw std::runtime_error("Cannot initialize regression decoder");
        }
    }
    ~Decoder() {
        value_->Uninitialize();
        WelsDestroyDecoder(value_);
    }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void verify(const EncodedVideoFrame& frame) {
        unsigned char* planes[3]{};
        SBufferInfo output{};
        output.uiInBsTimeStamp = frame.timestampMicroseconds;
        const auto status = value_->DecodeFrameNoDelay(frame.annexB.data(), static_cast<int>(frame.annexB.size()), planes, &output);
        require(status == dsErrorFree && output.iBufferStatus == 1 && output.UsrData.sSystemBuffer.iWidth == 64 &&
                    output.UsrData.sSystemBuffer.iHeight == 64 && output.uiOutYuvTimeStamp == frame.timestampMicroseconds,
                "Adaptive rate change broke the dependent video stream");
    }

  private:
    ISVCDecoder* value_ = nullptr;
};
void exercise(std::uint32_t initialRate) {
    SoftwareVideoEncoder encoder({64, 64, initialRate, 60});
    Decoder decoder;
    std::vector<std::uint8_t> pixels(64 * 64 * 4, 255);
    std::uint64_t timestamp = 0;
    const auto update = [&](std::uint32_t bitrate, std::uint32_t fps) {
        try {
            encoder.setRate(fps, bitrate);
            const bool key = timestamp == 0 || timestamp % (32 * 33333) == 0;
            auto frame = encoder.encode(pixels, 64 * 4, timestamp, key);
            require(frame.has_value() && (!key || frame->keyFrame), "Adaptive rate change failed to encode");
            decoder.verify(*frame);
            timestamp += 33333;
        } catch (...) {
            std::cerr << "initial_bitrate=" << initialRate << " requested_bitrate=" << bitrate << " fps=" << fps << '\n';
            throw;
        }
    };
    // All-layer updates use float arithmetic in OpenH264 2.6.0. Above 2^24,
    // 18,000,003 rounds UP to 18,000,004 and exceeds an exact integer ceiling.
    // Former tests only toggled round 4/8 Mbps values and missed this failure.
    for (const auto rate : {8000000u, 18000003u, 64000u, 24000000u, 16777215u, 16777217u, 16777219u,
                           33554431u, 33554435u, 67108863u, 67108871u, 99999999u, 100000000u, 64001u}) {
        update(rate, 60);
    }
    std::uint32_t seed = 0x451afe73;
    for (unsigned sample = 0; sample < 256; ++sample) {
        seed = seed * 1664525u + 1013904223u;
        update(64000u + seed % (100000000u - 64000u + 1u), std::array{10u, 30u, 60u, 120u}[sample % 4]);
    }
    // Exercise the actual pacing reductions and gradual recovery as well.
    std::uint32_t rate = 24000000;
    for (unsigned sample = 0; sample < 32; ++sample) {
        rate = rate > 512000 ? std::max(512000u, rate * 3 / 4) : 24000000;
        update(rate, 60);
        update(rate + rate / 10, 60);
    }
}
} // namespace
int main() {
    try {
        exercise(8000000);
        exercise(18000003);
        std::cout << "Adaptive non-round bitrate changes preserve a decodable stream\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
