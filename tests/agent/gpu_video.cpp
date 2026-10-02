#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <wels/codec_api.h>

#include "features/desktop/video.transport.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/media_encoder.transport.h"

int main() {
    using namespace ultraterimal::agent;
    try {
        DesktopCapture capture;
        std::optional<CapturedFrame> pixels;
        CaptureTiming timing;
        for (unsigned attempt = 0; attempt < 20 && !pixels; ++attempt) {
            pixels = capture.nextFrame(std::chrono::milliseconds(50), nullptr, &timing, false, false, true);
        }
        if (!pixels || !pixels->bgra.empty() || timing.mapMilliseconds || !capture.texture()) {
            throw std::runtime_error("GPU capture mapped CPU pixels");
        }
        VideoEncoder encoder({pixels->width, pixels->height, 16000000, 60, false}, capture.device());
        if (!encoder.gpu()) {
            std::cout << "GPU surface encoding unavailable; CPU/software fallback selected\n";
            return 77;
        }
        {
            MediaEncoder direct({pixels->width, pixels->height, 16000000, 60, false}, capture.device());
            auto frame = direct.encodeTexture(capture.texture(), 16667, true);
            if (!frame) {
                std::ostringstream reason;
                reason << "GPU adapter HRESULT 0x" << std::hex << static_cast<unsigned long>(frame.error());
                throw std::runtime_error(reason.str());
            }
            std::cout << "GPU adapter key frame: bytes=" << frame->bytes.size() << " key=" << frame->keyFrame << '\n';
        }
        ISVCDecoder* raw = nullptr;
        if (WelsCreateDecoder(&raw) != 0 || !raw) {
            throw std::runtime_error("Decoder creation failed");
        }
        struct Decoder {
            ISVCDecoder* value;
            ~Decoder() {
                value->Uninitialize();
                WelsDestroyDecoder(value);
            }
        } decoder{raw};
        SDecodingParam settings{};
        settings.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if (raw->Initialize(&settings) != 0) {
            throw std::runtime_error("Decoder initialization failed");
        }
        for (unsigned frame = 0; frame < 60; ++frame) {
            auto encoded = encoder.encodeTexture(capture.texture(), static_cast<std::uint64_t>(frame + 1) * 16667, frame % 20 == 0);
            if (!encoded || !encoder.gpu()) {
                throw std::runtime_error("GPU surface pool or encoding failed");
            }
            unsigned char* planes[3]{};
            SBufferInfo output{};
            if (raw->DecodeFrameNoDelay(encoded->annexB.data(), static_cast<int>(encoded->annexB.size()), planes, &output) != dsErrorFree || output.iBufferStatus != 1 ||
                output.UsrData.sSystemBuffer.iWidth != static_cast<int>(pixels->width) || output.UsrData.sSystemBuffer.iHeight != static_cast<int>(pixels->height)) {
                throw std::runtime_error("GPU frame is not independently decodable");
            }
        }
        const auto unavailable = encoder.encodeTexture(nullptr, 100 * 16667, true);
        if (unavailable || unavailable.error() != VideoEncodeError::kGpuUnavailable || encoder.gpu() || encoder.hardware()) {
            throw std::runtime_error("GPU failure did not select embedded software");
        }
        capture.readTexture(*pixels);
        const auto fallback = encoder.encode(pixels->bgra, static_cast<std::size_t>(pixels->width) * 4, 100 * 16667, true);
        unsigned char* planes[3]{};
        SBufferInfo output{};
        if (!fallback || !fallback->keyFrame || !fallback->baselineReset || raw->DecodeFrameNoDelay(fallback->annexB.data(), static_cast<int>(fallback->annexB.size()), planes, &output) != dsErrorFree || output.iBufferStatus != 1) {
            throw std::runtime_error("GPU failure did not rebuild a decodable software baseline");
        }
        std::cout << "60 GPU frames decoded without CPU readback; bounded tracked-surface reuse passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
