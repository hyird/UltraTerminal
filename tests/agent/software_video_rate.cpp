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
    explicit Decoder(int width = 64, int height = 64) : width_(width), height_(height) {
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
        require(status == dsErrorFree && output.iBufferStatus == 1 && output.UsrData.sSystemBuffer.iWidth == width_ &&
                    output.UsrData.sSystemBuffer.iHeight == height_ && output.uiOutYuvTimeStamp == frame.timestampMicroseconds,
                "Adaptive rate change broke the dependent video stream");
    }

  private:
    ISVCDecoder* value_ = nullptr;
    int width_;
    int height_;
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
    // Preserve historical non-round rate and overflow boundaries when changing
    // codecs. x264 uses integer kbit/s, not the caller's bits/s.
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
void exerciseFrameBudget() {
    SoftwareVideoEncoder encoder({320, 180, 8000000, 60});
    Decoder decoder(320, 180);
    std::vector<std::uint8_t> pixels(320 * 180 * 4);
    std::uint32_t random = 0x784bd367;
    std::uint64_t timestamp = 0;
    const auto measure = [&](std::uint32_t fps, std::uint32_t bitrate) {
        encoder.setRate(fps, bitrate);
        std::uint64_t bytes = 0;
        for (unsigned i = 0; i < 120; ++i) {
            for (auto& value : pixels) {
                random = random * 1664525u + 1013904223u;
                value = static_cast<std::uint8_t>(random >> 24);
            }
            auto frame = encoder.encode(pixels, 320 * 4, timestamp);
            require(frame.has_value(), "Adaptive frame budget skipped/failed a frame");
            if (i == 0 && timestamp) {
                require(!frame->keyFrame, "Rate changes must not rebuild the encoder");
            }
            decoder.verify(*frame);
            if (i >= 30) {
                bytes += frame->annexB.size();
            }
            timestamp += 1000000 / fps;
        }
        return bytes;
    };
    const auto highRate = measure(60, 8000000);
    const auto lowRate = measure(60, 256000);
    const auto slowCapture = measure(10, 1000000);
    const auto fastCapture = measure(60, 1000000);
    require(lowRate * 2 < highRate, "Bitrate reconfiguration did not lower encoded size");
    require(fastCapture * 5 < slowCapture * 4, "Capture FPS change did not adjust the per-frame rate budget");
    std::cout << "Frame budgets high=" << highRate << " low=" << lowRate << " slow=" << slowCapture << " fast=" << fastCapture << '\n';
}
void exerciseReliableCadence() {
    SoftwareVideoEncoder encoder({320, 180, 8000000, 30});
    Decoder decoder(320, 180);
    std::vector<std::uint8_t> pixels(320 * 180 * 4, 255);
    // More than two former five-second GOPs, including repeated and changed
    // pictures. Reliable delivery retains every reference until recovery.
    for (unsigned i = 0; i < 360; ++i) {
        if (i % 30 == 0) {
            std::fill(pixels.begin(), pixels.begin() + 320 * 4 * 30, i % 60 ? 32 : 240);
        }
        const auto frame = encoder.encode(pixels, 320 * 4, static_cast<std::uint64_t>(i) * 33333);
        require(frame.has_value() && frame->keyFrame == (i == 0), "Reliable stream inserted an unsolicited periodic keyframe");
        decoder.verify(*frame);
    }
    const auto refreshed = encoder.encode(pixels, 320 * 4, 360ull * 33333, true);
    require(refreshed.has_value() && refreshed->keyFrame, "Explicit recovery did not produce an independent keyframe");
    Decoder recovered(320, 180);
    recovered.verify(*refreshed);
    const auto following = encoder.encode(pixels, 320 * 4, 361ull * 33333);
    require(following.has_value() && !following->keyFrame, "Recovery unnecessarily retained keyframe output");
    recovered.verify(*following);
}
} // namespace
int main() {
    try {
        exercise(8000000);
        exercise(18000003);
        exerciseFrameBudget();
        exerciseReliableCadence();
        std::cout << "Adaptive non-round bitrate changes preserve a decodable stream\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
