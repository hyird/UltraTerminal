#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <wincodec.h>

#include <wrl/client.h>

#include "features/desktop/refinement.transport.h"

namespace {
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Refinement pixels or bounds failed");
    }
}
} // namespace
int main() {
    using namespace ultraterimal;
    using Microsoft::WRL::ComPtr;
    try {
        std::vector<std::uint8_t> pixels(256 * 128 * 4, 255);
        for (unsigned y = 0; y < 128; ++y) {
            for (unsigned x = 0; x < 256; ++x) {
                const auto offset = (y * 256 + x) * 4;
                if ((x % 16) < 3 && y % 16 < 10) {
                    pixels[offset] = 10;
                    pixels[offset + 1] = 20;
                    pixels[offset + 2] = 230;
                }
            }
        }
        auto detail = agent::refineVideoDetails(pixels, 256, 128);
        require(detail.patches_size() == 4);
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(initialized));
        struct Apartment {
            ~Apartment() { CoUninitialize(); }
        } apartment;
        ComPtr<IWICImagingFactory> factory;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
        for (const auto& patch : detail.patches()) {
            ComPtr<IWICStream> stream;
            require(SUCCEEDED(factory->CreateStream(&stream)));
            require(SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(patch.png().data())), static_cast<DWORD>(patch.png().size()))));
            ComPtr<IWICBitmapDecoder> decoder;
            require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
            ComPtr<IWICBitmapFrameDecode> frame;
            require(SUCCEEDED(decoder->GetFrame(0, &frame)));
            const auto& area = patch.destination();
            std::vector<std::uint8_t> restored(area.width() * area.height() * 4);
            require(SUCCEEDED(frame->CopyPixels(nullptr, area.width() * 4, static_cast<UINT>(restored.size()), restored.data())));
            for (unsigned y = 0; y < area.height(); ++y) {
                require(std::memcmp(restored.data() + y * area.width() * 4, pixels.data() + ((area.y() + y) * 256 + area.x()) * 4, area.width() * 4) == 0);
            }
        }
        wire::Envelope message;
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id("s");
        message.set_channel_id("v");
        message.set_sequence(1);
        message.set_generation(1);
        message.set_frame_id(1);
        message.set_base_frame_id(1);
        *message.mutable_video_refinement() = detail;
        require(protocol::validateMessage(message).has_value());
        message.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_width(129);
        require(!protocol::validateMessage(message));
        std::fill(pixels.begin(), pixels.end(), 255);
        require(agent::refineVideoDetails(pixels, 256, 128).patches_size() == 0);
        require(agent::refineVideoDetails({}, 256, 128).patches_size() == 0);
        std::cout << "Lossless detail PNGs preserve exact pixels; flat images and oversized input rejected\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
