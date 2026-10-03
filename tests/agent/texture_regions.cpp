#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "platform/windows/texture_regions.transport.h"

namespace {
using namespace ultraterimal;
using Microsoft::WRL::ComPtr;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
wire::Rectangle area(unsigned x, unsigned y, unsigned width, unsigned height) {
    wire::Rectangle result;
    result.set_x(x);
    result.set_y(y);
    result.set_width(width);
    result.set_height(height);
    return result;
}
} // namespace
int main(int argc, char** argv) {
    try {
        const bool hardware = argc == 2 && std::string_view(argv[1]) == "--hardware";
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr,
                                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)),
                "Create texture test device");
        agent::DesktopTextureRegions regions(device.Get());
        constexpr unsigned width = 1024, height = 512;
        std::vector<std::uint8_t> pixels(width * height * 4, 255);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), width * 4, 0};
        ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)), "Create baseline texture");
        require(regions.remember(texture.Get()) == S_OK, "GPU comparison not available on test device");
        pixels[(99 * width + 250) * 4] = 11;
        pixels[(511 * width + 1023) * 4 + 2] = 63;
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        auto changes = regions.changes(texture.Get());
        require(changes && changes->size() == 64 && (*changes)[9] == 1 && (*changes)[63] == 1 &&
                    std::count(changes->begin(), changes->end(), 1u) == 2,
                "GPU tile comparison missed pixels or overreported damage");
        const std::vector patches{area(128, 64, 128, 64), area(896, 448, 128, 64)};
        std::vector<std::uint8_t> copied(pixels.size(), 205), expected = copied;
        const auto read = regions.read(texture.Get(), patches, copied);
        require(read && *read == 2 * 128 * 64, "Sparse GPU copy did not remain bounded to two tiles");
        for (const auto& patch : patches) {
            for (unsigned y = patch.y(); y < patch.y() + patch.height(); ++y) {
                const auto offset = (static_cast<std::size_t>(y) * width + patch.x()) * 4;
                std::memcpy(expected.data() + offset, pixels.data() + offset, patch.width() * 4);
            }
        }
        require(copied == expected, "Atlas row pitch, edge geometry or untouched pixels changed");
        const std::vector invalid{patches[0], area(width, 0, 1, 1)};
        require(!regions.read(texture.Get(), invalid, copied) && copied == expected, "Malformed sparse read mutated output");
        // A failed/unsubmitted image does not retire its preceding baseline.
        pixels[0] = 24;
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        changes = regions.changes(texture.Get());
        require(changes && std::count(changes->begin(), changes->end(), 1u) == 3, "Unsubmitted GPU image advanced baseline");
        require(regions.remember(texture.Get()) == S_OK, "Commit GPU baseline");
        for (std::size_t index = 3; index < pixels.size(); index += 4) {
            pixels[index] = 0;
        }
        context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        changes = regions.changes(texture.Get());
        require(changes && std::ranges::none_of(*changes, [](auto value) { return value; }), "Undefined desktop alpha caused false changes");
        description.Width = 258;
        description.Height = 130;
        std::vector<std::uint8_t> edgePixels(258 * 130 * 4, 255);
        D3D11_SUBRESOURCE_DATA edgeInitial{edgePixels.data(), 258 * 4, 0};
        ComPtr<ID3D11Texture2D> edgeTexture;
        require(SUCCEEDED(device->CreateTexture2D(&description, &edgeInitial, &edgeTexture)) && regions.remember(edgeTexture.Get()) == S_OK, "Rebuild resized texture baseline");
        edgePixels[(129 * 258 + 257) * 4] = 0;
        context->UpdateSubresource(edgeTexture.Get(), 0, nullptr, edgePixels.data(), 258 * 4, 0);
        changes = regions.changes(edgeTexture.Get());
        require(changes && changes->size() == 9 && (*changes)[8] == 1 && std::count(changes->begin(), changes->end(), 1u) == 1, "Partial edge tile shader read outside geometry");
        const std::vector edgePatch{area(256, 128, 2, 2)};
        std::vector<std::uint8_t> edgeCopied(edgePixels.size(), 205);
        require(regions.read(edgeTexture.Get(), edgePatch, edgeCopied) == 4 && edgeCopied[(129 * 258 + 257) * 4] == 0 && edgeCopied[0] == 205, "Partial edge atlas read changed other rows");
        description.Width = width;
        description.Height = height;
        // Comparison remains optional on older feature levels; sparse staging
        // works independently of compute support and the shader compiler.
        ComPtr<ID3D11Device> olderDevice;
        ComPtr<ID3D11DeviceContext> olderContext;
        const D3D_FEATURE_LEVEL feature = D3D_FEATURE_LEVEL_10_0;
        require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                            &feature, 1, D3D11_SDK_VERSION, &olderDevice, nullptr, &olderContext)),
                "Create optional-comparison fallback");
        agent::DesktopTextureRegions older(olderDevice.Get());
        ComPtr<ID3D11Texture2D> olderTexture;
        require(SUCCEEDED(olderDevice->CreateTexture2D(&description, &initial, &olderTexture)), "Create fallback texture");
        require(older.remember(olderTexture.Get()) == S_FALSE && !older.changes(olderTexture.Get()), "Unsupported compute was mandatory");
        require(older.read(olderTexture.Get(), patches, copied).has_value(), "Sparse read required optional compute");
        std::cout << (hardware ? "Hardware" : "WARP") << " exact GPU tile changes, sparse atlas, alpha, unsubmitted baseline and optional-comparison fallback passed; read 16384/524288 pixels\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
