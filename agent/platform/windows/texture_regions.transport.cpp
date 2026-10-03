#include "texture_regions.transport.h"

#include <bit>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::string_view kCompareShader = R"(
Texture2D<float4> currentImage : register(t0);
Texture2D<float4> previousImage : register(t1);
RWStructuredBuffer<uint> changed : register(u0);
cbuffer Geometry : register(b0) { uint width; uint height; uint columns; uint unused; };
groupshared uint dirty;
[numthreads(16, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
    if (index == 0) dirty = 0;
    GroupMemoryBarrierWithGroupSync();
    bool different = false;
    for (uint y = thread.y; y < 64; y += 8) {
        for (uint x = thread.x; x < 128; x += 16) {
            uint2 p = group.xy * uint2(128, 64) + uint2(x, y);
            if (p.x < width && p.y < height)
                different = different || any(currentImage.Load(int3(p, 0)).rgb != previousImage.Load(int3(p, 0)).rgb);
        }
    }
    if (different) InterlockedOr(dirty, 1);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) changed[group.y * columns + group.x] = dirty;
}
)";
struct Mapping {
    ID3D11DeviceContext* context;
    ID3D11Resource* resource;
    ~Mapping() { context->Unmap(resource, 0); }
};
bool validTexture(const D3D11_TEXTURE2D_DESC& description) {
    return description.Width && description.Height && description.Width <= 16384 && description.Height <= 16384 &&
           static_cast<std::uint64_t>(description.Width) * description.Height * 4 <= 64 * 1024 * 1024 &&
           description.Format == DXGI_FORMAT_B8G8R8A8_UNORM && description.SampleDesc.Count == 1 && description.MipLevels == 1 && description.ArraySize == 1;
}
} // namespace
struct DesktopTextureRegions::State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> baseline, current, atlas;
    ComPtr<ID3D11ShaderResourceView> baselineView, currentView;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants, result, readback;
    ComPtr<ID3D11UnorderedAccessView> resultView;
    HMODULE compiler = nullptr;
    bool initialized = false;
    std::uint32_t resultCount = 0;
    explicit State(ID3D11Device* owner) : device(owner) {
        if (!device) {
            throw std::invalid_argument("Desktop texture comparison requires a device");
        }
        device->GetImmediateContext(&context);
    }
    ~State() {
        if (compiler) {
            FreeLibrary(compiler);
        }
    }
    bool initialize() {
        if (initialized) {
            return shader != nullptr;
        }
        initialized = true;
        if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            return false;
        }
        compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile = compiler ? std::bit_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) {
            return false;
        }
        ComPtr<ID3DBlob> bytecode;
        if (FAILED(compile(kCompareShader.data(), kCompareShader.size(), "desktop_tile_changes", nullptr, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, nullptr))) {
            return false;
        }
        return SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader));
    }
    HRESULT createResult(std::uint32_t count) {
        if (resultCount == count && result && readback && resultView && constants) {
            return S_OK;
        }
        result.Reset();
        readback.Reset();
        resultView.Reset();
        constants.Reset();
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = count * 4;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = 4;
        auto status = device->CreateBuffer(&description, nullptr, &result);
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = count;
        if (SUCCEEDED(status)) {
            status = device->CreateUnorderedAccessView(result.Get(), &view, &resultView);
        }
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        description.StructureByteStride = 0;
        if (SUCCEEDED(status)) {
            status = device->CreateBuffer(&description, nullptr, &readback);
        }
        description = {};
        description.ByteWidth = 16;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(status)) {
            status = device->CreateBuffer(&description, nullptr, &constants);
        }
        resultCount = SUCCEEDED(status) ? count : 0;
        return status;
    }
};
DesktopTextureRegions::DesktopTextureRegions(ID3D11Device* device) : state_(std::make_unique<State>(device)) {}
DesktopTextureRegions::~DesktopTextureRegions() = default;
HRESULT DesktopTextureRegions::remember(ID3D11Texture2D* texture) {
    if (!texture) {
        return E_INVALIDARG;
    }
    if (!state_->initialize()) {
        return S_FALSE;
    }
    D3D11_TEXTURE2D_DESC description{}, previous{};
    texture->GetDesc(&description);
    if (!validTexture(description)) {
        return E_INVALIDARG;
    }
    if (state_->baseline) {
        state_->baseline->GetDesc(&previous);
    }
    if (!state_->baseline || !state_->baselineView || previous.Width != description.Width || previous.Height != description.Height) {
        state_->baseline.Reset();
        state_->baselineView.Reset();
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = description.MiscFlags = 0;
        auto status = state_->device->CreateTexture2D(&description, nullptr, &state_->baseline);
        if (SUCCEEDED(status)) {
            status = state_->device->CreateShaderResourceView(state_->baseline.Get(), nullptr, &state_->baselineView);
        }
        if (FAILED(status)) {
            return status;
        }
    }
    state_->context->CopyResource(state_->baseline.Get(), texture);
    return S_OK;
}
std::expected<std::vector<std::uint32_t>, HRESULT> DesktopTextureRegions::changes(ID3D11Texture2D* texture) {
    if (!texture || !state_->baseline || !state_->initialize()) {
        return std::unexpected(S_FALSE);
    }
    D3D11_TEXTURE2D_DESC description{}, previous{};
    texture->GetDesc(&description);
    state_->baseline->GetDesc(&previous);
    if (!validTexture(description) || description.Width != previous.Width || description.Height != previous.Height) {
        return std::unexpected(S_FALSE);
    }
    if (state_->current.Get() != texture) {
        state_->currentView.Reset();
        const auto status = state_->device->CreateShaderResourceView(texture, nullptr, &state_->currentView);
        if (FAILED(status)) {
            return std::unexpected(status);
        }
        state_->current = texture;
    }
    const auto columns = (description.Width + 127) / 128;
    const auto rows = (description.Height + 63) / 64;
    const auto status = state_->createResult(columns * rows);
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    const std::uint32_t geometry[]{description.Width, description.Height, columns, 0};
    state_->context->UpdateSubresource(state_->constants.Get(), 0, nullptr, geometry, 0, 0);
    ID3D11ShaderResourceView* views[]{state_->currentView.Get(), state_->baselineView.Get()};
    ID3D11UnorderedAccessView* outputs[]{state_->resultView.Get()};
    ID3D11Buffer* constants[]{state_->constants.Get()};
    state_->context->CSSetShader(state_->shader.Get(), nullptr, 0);
    state_->context->CSSetShaderResources(0, 2, views);
    state_->context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    state_->context->CSSetConstantBuffers(0, 1, constants);
    state_->context->Dispatch(columns, rows, 1);
    views[0] = views[1] = nullptr;
    outputs[0] = nullptr;
    constants[0] = nullptr;
    state_->context->CSSetShaderResources(0, 2, views);
    state_->context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    state_->context->CSSetConstantBuffers(0, 1, constants);
    state_->context->CSSetShader(nullptr, nullptr, 0);
    state_->context->CopyResource(state_->readback.Get(), state_->result.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto mappedStatus = state_->context->Map(state_->readback.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mappedStatus)) {
        return std::unexpected(mappedStatus);
    }
    Mapping mapping{state_->context.Get(), state_->readback.Get()};
    std::vector<std::uint32_t> changed(columns * rows);
    std::memcpy(changed.data(), mapped.pData, changed.size() * 4);
    return changed;
}
std::expected<std::uint64_t, HRESULT> DesktopTextureRegions::read(ID3D11Texture2D* texture, std::span<const wire::Rectangle> regions, std::span<std::uint8_t> bgra) {
    if (!texture || regions.empty() || regions.size() > 32) {
        return std::unexpected(E_INVALIDARG);
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (!validTexture(description) || bgra.size() != static_cast<std::size_t>(description.Width) * description.Height * 4) {
        return std::unexpected(E_INVALIDARG);
    }
    for (const auto& area : regions) {
        if (!area.width() || !area.height() || area.width() > 128 || area.height() > 64 ||
            area.width() > description.Width || area.height() > description.Height ||
            area.x() > description.Width - area.width() || area.y() > description.Height - area.height()) {
            return std::unexpected(E_INVALIDARG);
        }
    }
    if (!state_->atlas) {
        auto atlas = description;
        atlas.Width = 128;
        atlas.Height = 64 * 32;
        atlas.MipLevels = atlas.ArraySize = 1;
        atlas.Usage = D3D11_USAGE_STAGING;
        atlas.BindFlags = atlas.MiscFlags = 0;
        atlas.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const auto status = state_->device->CreateTexture2D(&atlas, nullptr, &state_->atlas);
        if (FAILED(status)) {
            return std::unexpected(status);
        }
    }
    std::uint32_t index = 0;
    std::uint64_t pixels = 0;
    for (const auto& area : regions) {
        const D3D11_BOX box{area.x(), area.y(), 0, area.x() + area.width(), area.y() + area.height(), 1};
        state_->context->CopySubresourceRegion(state_->atlas.Get(), 0, 0, index++ * 64, 0, texture, 0, &box);
        pixels += static_cast<std::uint64_t>(area.width()) * area.height();
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto status = state_->context->Map(state_->atlas.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    Mapping mapping{state_->context.Get(), state_->atlas.Get()};
    index = 0;
    for (const auto& area : regions) {
        for (std::uint32_t row = 0; row < area.height(); ++row) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + (static_cast<std::size_t>(index) * 64 + row) * mapped.RowPitch;
            auto* destination = bgra.data() + (static_cast<std::size_t>(area.y() + row) * description.Width + area.x()) * 4;
            std::memcpy(destination, source, static_cast<std::size_t>(area.width()) * 4);
        }
        ++index;
    }
    return pixels;
}
} // namespace ultraterimal::agent
