#include "image_encoder.transport.h"

#include <array>
#include <wincodec.h>

#include <wrl/client.h>

namespace ultraterimal::agent {
struct DesktopPngEncoder::State {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT status = initialized;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    State() {
        if (SUCCEEDED(status)) {
            status = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        }
    }
    ~State() {
        factory.Reset();
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
    }
};
DesktopPngEncoder::DesktopPngEncoder() : state_(std::make_unique<State>()) {
    if (SUCCEEDED(state_->status)) {
        // WIC defers codec initialization until the first write and commit.
        // Complete it once with private pixels before optional work is admitted.
        const std::array<std::uint8_t, 128 * 64 * 4> pixels{};
        const auto initialized = encodeDetailPng(*this, pixels, 128, 64);
        if (!initialized) {
            state_->status = initialized.error();
        }
    }
}
DesktopPngEncoder::~DesktopPngEncoder() = default;

std::expected<std::string, HRESULT> encodeDetailPng(DesktopPngEncoder& pngEncoder, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    using Microsoft::WRL::ComPtr;
    if (!width || !height || width > 128 || height > 64 || bgra.size() != static_cast<std::size_t>(width) * height * 4) {
        return std::unexpected(E_INVALIDARG);
    }
    auto& resources = *pngEncoder.state_;
    if (FAILED(resources.status)) {
        return std::unexpected(resources.status);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IStream> stream;
    auto status = S_OK;
    if (SUCCEEDED(status)) {
        status = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    }
    if (SUCCEEDED(status)) {
        status = resources.factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(status)) {
        status = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(status)) {
        status = encoder->CreateNewFrame(&frame, nullptr);
    }
    if (SUCCEEDED(status)) {
        status = frame->Initialize(nullptr);
    }
    if (SUCCEEDED(status)) {
        status = frame->SetSize(width, height);
    }
    auto format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(status)) {
        status = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(status) && format != GUID_WICPixelFormat32bppBGRA) {
        status = E_NOTIMPL;
    }
    if (SUCCEEDED(status)) {
        status = frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()));
    }
    if (SUCCEEDED(status)) {
        status = frame->Commit();
    }
    if (SUCCEEDED(status)) {
        status = encoder->Commit();
    }
    STATSTG statistics{};
    if (SUCCEEDED(status)) {
        status = stream->Stat(&statistics, STATFLAG_NONAME);
    }
    if (SUCCEEDED(status) && (!statistics.cbSize.QuadPart || statistics.cbSize.QuadPart > 65536)) {
        status = E_UNEXPECTED;
    }
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    std::string bytes(static_cast<std::size_t>(statistics.cbSize.QuadPart), '\0');
    LARGE_INTEGER beginning{};
    status = stream->Seek(beginning, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (SUCCEEDED(status)) {
        status = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    }
    if (FAILED(status) || read != bytes.size()) {
        return std::unexpected(FAILED(status) ? status : E_UNEXPECTED);
    }
    return bytes;
}
} // namespace ultraterimal::agent
