#include "texture_regions.transport.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
std::uint32_t microseconds(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), 0, 60000000));
}
bool validRegion(const DesktopReadbackRegion& area, std::uint32_t width, std::uint32_t height) {
    return area.width && area.height && area.width <= 128 && area.height <= 64 &&
           area.width <= width && area.height <= height && area.x <= width - area.width && area.y <= height - area.height;
}
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
// Header: 32 uint masks, then every requested BGRA pixel packed as one uint.
// One group per ROI writes both products; no mask-only early-out is allowed.
constexpr std::uint32_t kMaximumPixels = 32 * 128 * 64;
constexpr std::uint32_t kComparisonWords = 32 + kMaximumPixels;
constexpr std::string_view kFusedShader = R"(
Texture2D<float4> currentAtlas : register(t0);
Texture2D<float4> previousImage : register(t1);
RWStructuredBuffer<uint> outputData : register(u0);
struct Roi { uint4 rectangle; uint4 storage; };
cbuffer Geometry : register(b0) { Roi regions[32]; };
groupshared uint dirty;
[numthreads(16, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
    Roi roi = regions[group.x];
    // Only group zero clears inactive mask words. Active groups write their
    // own mask, so there is no cross-group clear/write race.
    if (group.x == 0 && index >= roi.storage.y && index < 32) outputData[index] = 0;
    if (index == 0) dirty = 0;
    GroupMemoryBarrierWithGroupSync();
    bool different = false;
    for (uint y = thread.y; y < roi.rectangle.w; y += 8) {
        for (uint x = thread.x; x < roi.rectangle.z; x += 16) {
            uint4 pixel = (uint4)round(currentAtlas.Load(int3(x, group.x * 64 + y, 0)) * 255.0f);
            uint4 previous = (uint4)round(previousImage.Load(int3(roi.rectangle.xy + uint2(x, y), 0)) * 255.0f);
            different = different || any(pixel.rgb != previous.rgb);
            outputData[32 + roi.storage.x + y * roi.rectangle.z + x] = pixel.b | (pixel.g << 8) | (pixel.r << 16) | (pixel.a << 24);
        }
    }
    if (different) InterlockedOr(dirty, 1);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) outputData[group.x] = dirty;
}
)";
bool validComparisonTag(const DesktopReadbackTag& tag) {
    return tag.captureEpoch && tag.sourceSerial && tag.generation && tag.composedBase && tag.width && tag.height && tag.rotation <= 4;
}
bool sameComparisonContext(DesktopReadbackTag current, DesktopReadbackTag baseline) {
    current.sourceSerial = baseline.sourceSerial;
    return current == baseline;
}
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
    // Independent from the legacy synchronous atlas; never reuse in-flight data.
    ComPtr<ID3D11Texture2D> optionalAtlas;
    ComPtr<ID3D11Texture2D> comparisonBaseline, comparisonAtlas;
    ComPtr<ID3D11ShaderResourceView> comparisonBaselineView, comparisonAtlasView;
    ComPtr<ID3D11ComputeShader> comparisonShader;
    ComPtr<ID3D11Buffer> comparisonConstants, comparisonOutput, comparisonReadback;
    ComPtr<ID3D11UnorderedAccessView> comparisonOutputView;
    std::optional<DesktopReadbackTag> baselineTag;
    bool comparisonInitialized = false;
    std::optional<DesktopReadbackResult> pending;
    DesktopReadbackTiming pendingTiming;
    Clock::time_point submitted;
    bool retired = false;
    ComPtr<ID3D11ShaderResourceView> baselineView, currentView;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> constants, result, readback;
    ComPtr<ID3D11UnorderedAccessView> resultView;
    HMODULE compiler = nullptr;
    bool initialized = false;
    std::uint32_t resultCount = 0;
    explicit State(ID3D11Device* owner) : device(owner) {
        if (device) {
            device->GetImmediateContext(&context);
        }
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
        if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            return false;
        }
        return compileShader(kCompareShader, "desktop_tile_changes", shader);
    }
    bool compileShader(std::string_view source, const char* name, ComPtr<ID3D11ComputeShader>& target) {
        if (!device || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            return false;
        }
        if (!compiler) {
            compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        const auto compile = compiler ? std::bit_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) {
            return false;
        }
        ComPtr<ID3DBlob> bytecode;
        if (FAILED(compile(source.data(), source.size(), name, nullptr, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, nullptr))) {
            return false;
        }
        return SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &target));
    }
    bool initializeComparison() {
        if (!comparisonInitialized) {
            comparisonInitialized = true;
            compileShader(kFusedShader, "desktop_fused_roi", comparisonShader);
        }
        return comparisonShader != nullptr;
    }
    bool comparisonResources() {
        if (comparisonAtlas && comparisonAtlasView && comparisonConstants && comparisonOutput && comparisonReadback && comparisonOutputView) {
            return true;
        }
        comparisonAtlas.Reset();
        comparisonAtlasView.Reset();
        comparisonConstants.Reset();
        comparisonOutput.Reset();
        comparisonReadback.Reset();
        comparisonOutputView.Reset();
        D3D11_TEXTURE2D_DESC atlasDescription{};
        atlasDescription.Width = 128;
        atlasDescription.Height = 64 * 32;
        atlasDescription.MipLevels = atlasDescription.ArraySize = atlasDescription.SampleDesc.Count = 1;
        atlasDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        atlasDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        auto status = device->CreateTexture2D(&atlasDescription, nullptr, &comparisonAtlas);
        if (SUCCEEDED(status)) {
            status = device->CreateShaderResourceView(comparisonAtlas.Get(), nullptr, &comparisonAtlasView);
        }
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = kComparisonWords * 4;
        buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        buffer.StructureByteStride = 4;
        if (SUCCEEDED(status)) {
            status = device->CreateBuffer(&buffer, nullptr, &comparisonOutput);
        }
        if (SUCCEEDED(status)) {
            status = device->CreateUnorderedAccessView(comparisonOutput.Get(), nullptr, &comparisonOutputView);
        }
        buffer.Usage = D3D11_USAGE_STAGING;
        buffer.BindFlags = buffer.MiscFlags = buffer.StructureByteStride = 0;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(status)) {
            status = device->CreateBuffer(&buffer, nullptr, &comparisonReadback);
        }
        buffer = {};
        buffer.ByteWidth = 32 * 32;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (SUCCEEDED(status)) {
            status = device->CreateBuffer(&buffer, nullptr, &comparisonConstants);
        }
        return SUCCEEDED(status);
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
bool DesktopReadbackResult::applyTo(std::span<std::uint8_t> destination) const {
    if (!tag.width || !tag.height || tag.width > 16384 || tag.height > 16384 ||
        static_cast<std::uint64_t>(tag.width) * tag.height * 4 > 64 * 1024 * 1024 ||
        static_cast<std::uint64_t>(tag.width) * tag.height * 4 != destination.size() || regions.empty() || regions.size() > 32) {
        return false;
    }
    std::uint64_t count = 0;
    for (const auto& area : regions) {
        if (!validRegion(area, tag.width, tag.height)) {
            return false;
        }
        count += static_cast<std::uint64_t>(area.width) * area.height;
    }
    if (count > kMaximumPixels || count != pixels || count * 4 != bgra.size() ||
        (comparison && (!validComparisonTag(tag) || !sameComparisonContext(tag, comparison->baselineTag) || !validComparisonTag(comparison->baselineTag) || tag.sourceSerial < comparison->baselineTag.sourceSerial ||
                        comparison->changed.size() != regions.size() || std::ranges::any_of(comparison->changed, [](auto mask) { return mask > 1; })))) {
        return false;
    }
    std::size_t offset = 0;
    for (const auto& area : regions) {
        for (std::uint32_t row = 0; row < area.height; ++row) {
            auto* output = destination.data() + (static_cast<std::size_t>(area.y + row) * tag.width + area.x) * 4;
            std::memcpy(output, bgra.data() + offset, area.width * 4);
            offset += area.width * 4;
        }
    }
    return true;
}
std::expected<DesktopReadbackSubmit, HRESULT> DesktopTextureRegions::submit(ID3D11Texture2D* texture, const DesktopReadbackTag& tag, std::span<const DesktopReadbackRegion> regions) {
    if (state_->pending) {
        return DesktopReadbackSubmit::kBusy;
    }
    if (!state_->device || !state_->context) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    if (!texture || !tag.captureEpoch || !tag.sourceSerial || regions.empty() || regions.size() > 32) {
        return std::unexpected(E_INVALIDARG);
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    ComPtr<ID3D11Device> sourceDevice;
    texture->GetDevice(&sourceDevice);
    if (sourceDevice.Get() != state_->device.Get() || !validTexture(description) || tag.width != description.Width || tag.height != description.Height) {
        return std::unexpected(E_INVALIDARG);
    }
    DesktopReadbackResult result;
    result.tag = tag;
    result.regions.assign(regions.begin(), regions.end());
    for (const auto& area : regions) {
        if (!validRegion(area, tag.width, tag.height)) {
            return std::unexpected(E_INVALIDARG);
        }
        result.pixels += static_cast<std::uint64_t>(area.width) * area.height;
    }
    result.bgra.resize(static_cast<std::size_t>(result.pixels) * 4);
    if (!state_->optionalAtlas) {
        description.Width = 128;
        description.Height = 64 * 32;
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = description.MiscFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const auto status = state_->device->CreateTexture2D(&description, nullptr, &state_->optionalAtlas);
        if (FAILED(status)) {
            // Allocation/unsupported optional resources must not stop video.
            if (status == E_OUTOFMEMORY || status == E_INVALIDARG || status == DXGI_ERROR_UNSUPPORTED) {
                return DesktopReadbackSubmit::kUnavailable;
            }
            return std::unexpected(status);
        }
    }
    state_->pending = std::move(result);
    state_->retired = false;
    state_->pendingTiming = {};
    state_->submitted = Clock::now();
    std::uint32_t index = 0;
    for (const auto& area : regions) {
        const D3D11_BOX box{area.x, area.y, 0, area.x + area.width, area.y + area.height, 1};
        state_->context->CopySubresourceRegion(state_->optionalAtlas.Get(), 0, 0, index++ * 64, 0, texture, 0, &box);
    }
    // DO_NOT_WAIT Map need not submit queued copies. Submit once here, without
    // waiting for GPU completion; Pending probes must never repeat this Flush.
    state_->context->Flush();
    state_->pendingTiming.copyDispatchMicroseconds = microseconds(state_->submitted, Clock::now());
    return DesktopReadbackSubmit::kSubmitted;
}
std::expected<DesktopReadbackCompletion, HRESULT> DesktopTextureRegions::tryComplete() {
    DesktopReadbackCompletion completion;
    if (!state_->pending) {
        return completion;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto started = Clock::now();
    ID3D11Resource* resource = state_->pending->comparison ? static_cast<ID3D11Resource*>(state_->comparisonReadback.Get()) : state_->optionalAtlas.Get();
    const auto status = state_->context->Map(resource, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    const auto ready = Clock::now();
    const auto elapsed = microseconds(started, ready);
    state_->pendingTiming.mapMicroseconds = std::min(60000000u, state_->pendingTiming.mapMicroseconds.value_or(0) + elapsed);
    completion.timing = state_->pendingTiming;
    if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        completion.status = DesktopReadbackStatus::kPending;
        return completion;
    }
    if (FAILED(status)) {
        // Failure is not proof of physical completion. Preserve the slot and
        // retire its output until completion or capture/device destruction.
        state_->retired = true;
        return std::unexpected(status);
    }
    Mapping mapping{state_->context.Get(), resource};
    completion.timing.readbackReadyMicroseconds = microseconds(state_->submitted, ready);
    if (state_->retired) {
        completion.status = DesktopReadbackStatus::kRetired;
    } else {
        const auto rowsStarted = Clock::now();
        if (state_->pending->comparison) {
            const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
            const auto count = state_->pending->regions.size();
            std::uint64_t pixels = 0;
            D3D11_BUFFER_DESC storage{};
            state_->comparisonReadback->GetDesc(&storage);
            bool valid = words && count && count <= 32 && storage.ByteWidth >= 32 * 4;
            for (const auto& area : state_->pending->regions) {
                valid = valid && validRegion(area, state_->pending->tag.width, state_->pending->tag.height);
                pixels += static_cast<std::uint64_t>(area.width) * area.height;
            }
            const auto copiedBytes = (32 + pixels) * 4;
            valid = valid && pixels <= kMaximumPixels && pixels == state_->pending->pixels && pixels * 4 == state_->pending->bgra.size() &&
                    copiedBytes <= storage.ByteWidth && state_->pendingTiming.gpuBytes == copiedBytes && state_->pending->comparison->changed.size() == count &&
                    validComparisonTag(state_->pending->tag) && validComparisonTag(state_->pending->comparison->baselineTag) &&
                    sameComparisonContext(state_->pending->tag, state_->pending->comparison->baselineTag);
            if (valid) {
                for (std::size_t index = 0; index < 32; ++index) {
                    valid = valid && words[index] <= 1 && (index < count || words[index] == 0);
                }
            }
            if (!valid) {
                state_->pending.reset();
                return std::unexpected(E_INVALIDARG);
            }
            // Bounds and every mask are validated before writing owned output.
            std::copy_n(words, count, state_->pending->comparison->changed.begin());
            for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                const auto value = words[32 + pixel];
                for (unsigned channel = 0; channel < 4; ++channel) {
                    state_->pending->bgra[pixel * 4 + channel] = static_cast<std::uint8_t>(value >> (channel * 8));
                }
            }
        } else {
            std::size_t offset = 0;
            std::uint32_t index = 0;
            for (const auto& area : state_->pending->regions) {
                for (std::uint32_t row = 0; row < area.height; ++row) {
                    const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + (static_cast<std::size_t>(index) * 64 + row) * mapped.RowPitch;
                    std::memcpy(state_->pending->bgra.data() + offset, source, area.width * 4);
                    offset += area.width * 4;
                }
                ++index;
            }
        }
        completion.timing.rowCopyMicroseconds = microseconds(rowsStarted, Clock::now());
        completion.status = DesktopReadbackStatus::kReady;
        completion.result = std::move(state_->pending);
    }
    state_->pending.reset();
    return completion;
}
void DesktopTextureRegions::retire() {
    if (state_->pending) {
        state_->retired = true;
    }
}
std::expected<DesktopTextureBaselineResult, HRESULT> DesktopTextureRegions::rememberBaseline(ID3D11Texture2D* texture, const DesktopReadbackTag& tag) {
    DesktopTextureBaselineResult result;
    if (state_->pending) {
        result.status = DesktopTextureBaselineStatus::kBusy;
        return result;
    }
    if (!texture || !validComparisonTag(tag) || !state_->device || !state_->context) {
        return result;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.Get() != state_->device.Get() || !validTexture(description) || tag.width != description.Width || tag.height != description.Height ||
        (state_->baselineTag && tag.captureEpoch == state_->baselineTag->captureEpoch && tag.generation == state_->baselineTag->generation && tag.composedBase <= state_->baselineTag->composedBase)) {
        return result;
    }
    const auto started = Clock::now();
    if (!state_->initializeComparison()) {
        return result;
    }
    D3D11_TEXTURE2D_DESC previous{};
    if (state_->comparisonBaseline) {
        state_->comparisonBaseline->GetDesc(&previous);
    }
    if (!state_->comparisonBaselineView || previous.Width != description.Width || previous.Height != description.Height) {
        ComPtr<ID3D11Texture2D> baseline;
        ComPtr<ID3D11ShaderResourceView> view;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = description.MiscFlags = 0;
        if (FAILED(state_->device->CreateTexture2D(&description, nullptr, &baseline)) || FAILED(state_->device->CreateShaderResourceView(baseline.Get(), nullptr, &view))) {
            return result;
        }
        state_->comparisonBaseline = std::move(baseline);
        state_->comparisonBaselineView = std::move(view);
    }
    state_->context->CopyResource(state_->comparisonBaseline.Get(), texture);
    // The later comparison flushes the same immediate-context queue. Its
    // ordering preserves this snapshot without flushing every primary frame.
    state_->baselineTag = tag;
    result.status = DesktopTextureBaselineStatus::kQueued;
    result.copyDispatchMicroseconds = microseconds(started, Clock::now());
    result.copiedBytes = static_cast<std::uint64_t>(tag.width) * tag.height * 4;
    return result;
}
void DesktopTextureRegions::invalidateBaseline() {
    state_->baselineTag.reset();
    if (state_->pending && state_->pending->comparison) {
        retire();
    }
}
std::expected<DesktopReadbackSubmit, HRESULT> DesktopTextureRegions::submitComparison(ID3D11Texture2D* texture, const DesktopReadbackTag& tag, const DesktopReadbackTag& expectedBaseline, std::span<const DesktopReadbackRegion> regions) {
    if (state_->pending) {
        return DesktopReadbackSubmit::kBusy;
    }
    if (!texture || !state_->device || !state_->context || !validComparisonTag(tag) || !validComparisonTag(expectedBaseline) ||
        !state_->baselineTag || expectedBaseline != *state_->baselineTag || !sameComparisonContext(tag, expectedBaseline) || tag.sourceSerial < expectedBaseline.sourceSerial) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.Get() != state_->device.Get() || !validTexture(description) || tag.width != description.Width || tag.height != description.Height) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    if (regions.empty() || regions.size() > 32) {
        return std::unexpected(E_INVALIDARG);
    }
    DesktopReadbackResult result;
    result.tag = tag;
    result.regions.assign(regions.begin(), regions.end());
    result.comparison = DesktopReadbackComparison{expectedBaseline, std::vector<std::uint32_t>(regions.size())};
    std::array<std::array<std::uint32_t, 8>, 32> geometry{};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& area = regions[index];
        if (!validRegion(area, tag.width, tag.height)) {
            return std::unexpected(E_INVALIDARG);
        }
        geometry[index] = {area.x, area.y, area.width, area.height, static_cast<std::uint32_t>(result.pixels), static_cast<std::uint32_t>(regions.size()), 0, 0};
        result.pixels += static_cast<std::uint64_t>(area.width) * area.height;
    }
    if (result.pixels > kMaximumPixels) {
        return std::unexpected(E_INVALIDARG);
    }
    const auto started = Clock::now();
    if (!state_->initializeComparison() || !state_->comparisonResources()) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    result.bgra.resize(static_cast<std::size_t>(result.pixels) * 4);
    state_->pending = std::move(result);
    state_->retired = false;
    state_->submitted = started;
    state_->pendingTiming = {};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& area = regions[index];
        const D3D11_BOX box{area.x, area.y, 0, area.x + area.width, area.y + area.height, 1};
        state_->context->CopySubresourceRegion(state_->comparisonAtlas.Get(), 0, 0, static_cast<UINT>(index) * 64, 0, texture, 0, &box);
    }
    state_->context->UpdateSubresource(state_->comparisonConstants.Get(), 0, nullptr, geometry.data(), 0, 0);
    ID3D11ShaderResourceView* views[]{state_->comparisonAtlasView.Get(), state_->comparisonBaselineView.Get()};
    ID3D11UnorderedAccessView* outputs[]{state_->comparisonOutputView.Get()};
    ID3D11Buffer* constants[]{state_->comparisonConstants.Get()};
    state_->context->CSSetShader(state_->comparisonShader.Get(), nullptr, 0);
    state_->context->CSSetShaderResources(0, 2, views);
    state_->context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    state_->context->CSSetConstantBuffers(0, 1, constants);
    state_->context->Dispatch(static_cast<UINT>(regions.size()), 1, 1);
    views[0] = views[1] = nullptr;
    outputs[0] = nullptr;
    constants[0] = nullptr;
    state_->context->CSSetShaderResources(0, 2, views);
    state_->context->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
    state_->context->CSSetConstantBuffers(0, 1, constants);
    state_->context->CSSetShader(nullptr, nullptr, 0);
    const auto copiedBytes = static_cast<UINT>((32 + state_->pending->pixels) * 4);
    const D3D11_BOX prefix{0, 0, 0, copiedBytes, 1, 1};
    // Buffer box coordinates are bytes (not texels). Capacity is unchanged;
    // only this job's fully written header/pixels cross to staging.
    state_->context->CopySubresourceRegion(state_->comparisonReadback.Get(), 0, 0, 0, 0, state_->comparisonOutput.Get(), 0, &prefix);
    state_->pendingTiming.gpuBytes = copiedBytes;
    state_->context->Flush(); // Exactly once on submission; probes do not Flush.
    state_->pendingTiming.copyDispatchMicroseconds = microseconds(started, Clock::now());
    return DesktopReadbackSubmit::kSubmitted;
}
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
    if (state_->pending) {
        return std::unexpected(DXGI_ERROR_WAS_STILL_DRAWING);
    }
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
std::expected<std::uint64_t, HRESULT> DesktopTextureRegions::read(ID3D11Texture2D* texture, std::span<const wire::Rectangle> regions, std::span<std::uint8_t> bgra, DesktopReadbackTiming* timing) {
    if (state_->pending) {
        return std::unexpected(DXGI_ERROR_WAS_STILL_DRAWING);
    }
    if (!state_->device || !state_->context) {
        return std::unexpected(S_FALSE);
    }
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
    const auto mapStarted = Clock::now();
    const auto status = state_->context->Map(state_->atlas.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (timing) {
        timing->mapMicroseconds = microseconds(mapStarted, Clock::now());
    }
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    Mapping mapping{state_->context.Get(), state_->atlas.Get()};
    const auto copyStarted = Clock::now();
    index = 0;
    for (const auto& area : regions) {
        for (std::uint32_t row = 0; row < area.height(); ++row) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) + (static_cast<std::size_t>(index) * 64 + row) * mapped.RowPitch;
            auto* destination = bgra.data() + (static_cast<std::size_t>(area.y() + row) * description.Width + area.x()) * 4;
            std::memcpy(destination, source, static_cast<std::size_t>(area.width()) * 4);
        }
        ++index;
    }
    if (timing) {
        timing->rowCopyMicroseconds = microseconds(copyStarted, Clock::now());
    }
    return pixels;
}
} // namespace ultraterimal::agent
