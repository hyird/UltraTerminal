#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "platform/windows/texture_regions.transport.h"

namespace {
using namespace ultraterimal;
using Microsoft::WRL::ComPtr;
void require(bool value, const char* reason);
agent::DesktopReadbackCompletion scheduledComplete(agent::DesktopTextureRegions& regions);
void fusedComparison(ID3D11Device* device, ID3D11DeviceContext* context, unsigned width, unsigned height) {
    using namespace agent;
    std::vector<std::uint8_t> baseline(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t pixel = 0; pixel < baseline.size() / 4; ++pixel) {
        baseline[pixel * 4] = static_cast<std::uint8_t>(pixel);
        baseline[pixel * 4 + 1] = static_cast<std::uint8_t>(pixel * 3);
        baseline[pixel * 4 + 2] = static_cast<std::uint8_t>(pixel * 73 + 19);
        baseline[pixel * 4 + 3] = static_cast<std::uint8_t>(pixel * 41 + 11);
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    // Source need not itself be an SRV: only copied ROIs supply currentAtlas.
    D3D11_SUBRESOURCE_DATA initial{baseline.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)), "Create fused mutable source");
    DesktopTextureRegions regions(device);
    DesktopReadbackTag baseTag{9, 31, 8, 7, width, height, -640, 11, 2}, tag = baseTag;
    ++tag.sourceSerial;
    const std::vector<DesktopReadbackRegion> patches{{0, 0, 128, 2}, {128, 0, 128, 2}, {width - 1, height - 1, 1, 1}};
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kUnavailable, "Unknown baseline gave equality proof");
    require(regions.remember(texture.Get()) == S_OK && regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kUnavailable,
            "Legacy remember implicitly committed comparison key");
    const auto remembered = regions.rememberBaseline(texture.Get(), baseTag);
    require(remembered && remembered->status == DesktopTextureBaselineStatus::kQueued && remembered->copyDispatchMicroseconds && remembered->copiedBytes == baseline.size(), "Independent keyed baseline unavailable");
    require(regions.rememberBaseline(texture.Get(), baseTag)->status == DesktopTextureBaselineStatus::kUnavailable, "Same composed base recommitted");
    const auto verify = [&](const DesktopReadbackCompletion& completed, const std::vector<std::uint8_t>& current, const std::vector<DesktopReadbackRegion>& areas) {
        require(completed.status == DesktopReadbackStatus::kReady && completed.result && completed.result->comparison && completed.result->tag == tag && completed.result->comparison->baselineTag == baseTag,
                "Fused result lost distinct current/baseline identity");
        const auto& result = *completed.result;
        std::uint64_t actualPixels = 0;
        for (const auto& area : areas) {
            actualPixels += static_cast<std::uint64_t>(area.width) * area.height;
        }
        require(result.pixels == actualPixels && completed.timing.gpuBytes == 4 * (32 + actualPixels), "Fused GPU copy did not match actual prefix");
        std::cout << "fused_prefix regions=" << areas.size() << " pixels=" << actualPixels << " gpu_bytes=" << *completed.timing.gpuBytes << '\n';
        require(result.comparison->changed.size() == areas.size() && result.regions.size() == areas.size() && result.bgra.size() == result.pixels * 4, "Fused storage incomplete");
        std::vector<std::uint8_t> restored(baseline.size(), 205), expected = restored;
        for (std::size_t index = 0; index < areas.size(); ++index) {
            const auto& area = areas[index];
            bool changed = false;
            for (unsigned row = 0; row < area.height; ++row) {
                const auto offset = (static_cast<std::size_t>(area.y + row) * width + area.x) * 4;
                std::memcpy(expected.data() + offset, current.data() + offset, area.width * 4);
                for (unsigned column = 0; column < area.width; ++column) {
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        changed = changed || current[offset + column * 4 + channel] != baseline[offset + column * 4 + channel];
                    }
                }
            }
            require(result.comparison->changed[index] == static_cast<unsigned>(changed), "Fused mask disagreed with exact RGB oracle");
        }
        require(result.applyTo(restored) && restored == expected, "All RGBA values/unchanged ROIs/origin/rotation/sparse outside pixels not preserved");
        auto malformed = result;
        malformed.comparison->changed.pop_back();
        require(!malformed.applyTo(restored) && restored == expected, "Incomplete mask partially wrote pixels");
        malformed = result;
        malformed.comparison->changed[0] = 2;
        require(!malformed.applyTo(restored) && restored == expected, "Invalid changed bit accepted");
        malformed = result;
        ++malformed.comparison->baselineTag.rotation;
        require(!malformed.applyTo(restored), "Malformed baseline geometry accepted");
        malformed = result;
        malformed.pixels = 1ull << 63;
        require(!malformed.applyTo(restored), "Overflowing packed pixel storage accepted");
    };
    auto current = baseline;
    for (std::size_t offset = 3; offset < current.size(); offset += 4) {
        ++current[offset]; // All 256 alpha byte values, no RGB changes.
    }
    context->UpdateSubresource(texture.Get(), 0, nullptr, current.data(), width * 4, 0);
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit alpha-only fused comparison");
    require(regions.submit(texture.Get(), tag, patches) == DesktopReadbackSubmit::kBusy && regions.rememberBaseline(texture.Get(), baseTag)->status == DesktopTextureBaselineStatus::kBusy,
            "Ordinary submit/baseline reused comparison physical slot");
    std::vector<std::uint8_t> overwritten(current.size(), 37);
    context->UpdateSubresource(texture.Get(), 0, nullptr, overwritten.data(), width * 4, 0);
    auto completed = scheduledComplete(regions);
    verify(completed, current, patches);
    require(std::ranges::none_of(completed.result->comparison->changed, [](auto bit) { return bit; }), "Alpha-only change incorrectly marked RGB");
    // Changed RGB lies in ROI1 only; nevertheless ROI0 and all RGBA must survive.
    ++current[(width + 129) * 4 + 2];
    context->UpdateSubresource(texture.Get(), 0, nullptr, current.data(), width * 4, 0);
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit one RGB pixel");
    completed = scheduledComplete(regions);
    verify(completed, current, patches);
    require(completed.result->comparison->changed == std::vector<std::uint32_t>({0, 1, 0}), "Single pixel changed wrong ROI bit");
    // Comparison, ordinary readback and legacy remember must not advance key.
    require(regions.remember(texture.Get()) == S_OK, "Legacy remember coexists");
    require(regions.submit(texture.Get(), tag, patches) == DesktopReadbackSubmit::kSubmitted && regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kBusy,
            "Fused submit used a second independent busy flag");
    require(!scheduledComplete(regions).result->comparison, "Ordinary result fabricated comparison");
    for (unsigned field = 0; field < 9; ++field) {
        auto stale = baseTag;
        switch (field) {
        case 0:
            ++stale.captureEpoch;
            break;
        case 1:
            ++stale.sourceSerial;
            break;
        case 2:
            ++stale.generation;
            break;
        case 3:
            ++stale.composedBase;
            break;
        case 4:
            ++stale.width;
            break;
        case 5:
            ++stale.height;
            break;
        case 6:
            ++stale.desktopLeft;
            break;
        case 7:
            ++stale.desktopTop;
            break;
        case 8:
            ++stale.rotation;
            break;
        }
        require(regions.submitComparison(texture.Get(), tag, stale, patches) == DesktopReadbackSubmit::kUnavailable, "Bad expected baseline accepted");
        if (field != 1) {
            stale = tag;
            switch (field) {
            case 0:
                ++stale.captureEpoch;
                break;
            case 2:
                ++stale.generation;
                break;
            case 3:
                ++stale.composedBase;
                break;
            case 4:
                ++stale.width;
                break;
            case 5:
                ++stale.height;
                break;
            case 6:
                ++stale.desktopLeft;
                break;
            case 7:
                ++stale.desktopTop;
                break;
            case 8:
                ++stale.rotation;
                break;
            }
            require(regions.submitComparison(texture.Get(), stale, baseTag, patches) == DesktopReadbackSubmit::kUnavailable, "Cross-context current accepted");
        }
    }
    require(regions.submitComparison(texture.Get(), {}, baseTag, patches) == DesktopReadbackSubmit::kUnavailable, "Bad zero current tag accepted");
    auto staleSerial = tag;
    staleSerial.sourceSerial = baseTag.sourceSerial - 1;
    require(regions.submitComparison(texture.Get(), staleSerial, baseTag, patches) == DesktopReadbackSubmit::kUnavailable, "Backward source serial accepted");
    ComPtr<ID3D11Device> otherDevice;
    ComPtr<ID3D11Texture2D> otherTexture;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &otherDevice, nullptr, nullptr)) &&
                SUCCEEDED(otherDevice->CreateTexture2D(&description, &initial, &otherTexture)),
            "Create foreign source device");
    require(regions.submitComparison(otherTexture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kUnavailable &&
                regions.rememberBaseline(otherTexture.Get(), tag)->status == DesktopTextureBaselineStatus::kUnavailable,
            "Foreign context source accepted");
    const std::vector<DesktopReadbackRegion> invalid{{0, 0, 129, 64}}, overflowing{{0xffffffffu, 0, 1, 1}}, tooTall{{0, 0, 1, 65}};
    require(!regions.submitComparison(texture.Get(), tag, baseTag, {}) && !regions.submitComparison(texture.Get(), tag, baseTag, invalid) &&
                !regions.submitComparison(texture.Get(), tag, baseTag, overflowing) && !regions.submitComparison(texture.Get(), tag, baseTag, tooTall),
            "ROI bounds bypassed");
    std::vector<DesktopReadbackRegion> whole;
    for (unsigned y = 0; y < 256; y += 64) {
        for (unsigned x = 0; x < 1024; x += 128) {
            whole.push_back({x, y, 128, 64});
        }
    }
    for (unsigned count : {8u, 32u, 1u, 2u, 32u, 3u, 1u}) {
        const std::vector<DesktopReadbackRegion> areas(whole.begin(), whole.begin() + count);
        require(regions.submitComparison(texture.Get(), tag, baseTag, areas) == DesktopReadbackSubmit::kSubmitted, "Submit 64ki/256ki fused limit");
        verify(scheduledComplete(regions), current, areas);
    }
    // Reuse the large staging allocation for zero BGRA, alpha-only, and a
    // differently-sized region/count prefix. No stale tail may be delivered.
    std::fill(current.begin(), current.end(), 0);
    context->UpdateSubresource(texture.Get(), 0, nullptr, current.data(), width * 4, 0);
    for (unsigned columns : {64u, 128u}) {
        for (unsigned rows : {1u, 64u}) {
            const std::vector<DesktopReadbackRegion> compactAreas{{0, 0, columns, rows}};
            require(regions.submitComparison(texture.Get(), tag, baseTag, compactAreas) == DesktopReadbackSubmit::kSubmitted, "Submit small zero prefix");
            verify(scheduledComplete(regions), current, compactAreas);
        }
    }
    current = baseline;
    for (std::size_t offset = 3; offset < current.size(); offset += 4) {
        current[offset] = 0;
    }
    context->UpdateSubresource(texture.Get(), 0, nullptr, current.data(), width * 4, 0);
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit reused alpha-only prefix");
    const auto alphaOnly = scheduledComplete(regions);
    verify(alphaOnly, current, patches);
    require(std::ranges::none_of(alphaOnly.result->comparison->changed, [](auto bit) { return bit; }), "Reused alpha prefix retained old masks");
    whole.push_back(patches.front());
    require(!regions.submitComparison(texture.Get(), tag, baseTag, whole), "33rd ROI exceeded storage cap");
    require(regions.submitComparison(texture.Get(), tag, baseTag, std::span(whole).first(32)) == DesktopReadbackSubmit::kSubmitted, "Submit large prefix before baseline invalidation");
    regions.invalidateBaseline();
    require(regions.submit(texture.Get(), tag, patches) == DesktopReadbackSubmit::kBusy && regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kBusy,
            "Retirement freed physical slot without drain");
    auto retired = scheduledComplete(regions);
    require(retired.status == DesktopReadbackStatus::kRetired && !retired.result && !retired.timing.rowCopyMicroseconds && retired.timing.gpuBytes == 1048704, "Retired large prefix escaped or prematurely released");
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kUnavailable, "Cleared baseline remained valid");
    ++baseTag.composedBase;
    require(regions.rememberBaseline(texture.Get(), baseTag)->status == DesktopTextureBaselineStatus::kQueued, "Explicit new base commit failed");
    tag = baseTag;
    require(regions.submitComparison(texture.Get(), tag, baseTag, patches) == DesktopReadbackSubmit::kSubmitted, "Same source static comparison failed");
    completed = scheduledComplete(regions);
    require(completed.result && completed.timing.gpuBytes == 2180 && completed.result->bgra.size() == 513 * 4 && std::ranges::none_of(completed.result->comparison->changed, [](auto bit) { return bit; }), "New composed baseline/small prefix after drain was not used");
    std::cout << "fused exact RGBA256/masks RGB-alpha/onepixel/allrequested pixels/64ki-256ki/sharedslot/retire-drain/key barriers passed"
              << " submit_cpu_us=" << completed.timing.copyDispatchMicroseconds.value_or(0) << " observed_ready_us=" << completed.timing.readbackReadyMicroseconds.value_or(0)
              << " map_cpu_us=" << completed.timing.mapMicroseconds.value_or(0) << " unpack_cpu_us=" << completed.timing.rowCopyMicroseconds.value_or(0) << '\n';
    // Queue a 2560x1600 independent full GPU baseline; never CPU full readback.
    description.Width = 2560;
    description.Height = 1600;
    ComPtr<ID3D11Texture2D> desktop;
    require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &desktop)), "Create full GPU baseline input");
    DesktopReadbackTag desktopTag{9, 33, 8, 9, 2560, 1600};
    const auto start = std::chrono::steady_clock::now();
    auto full = regions.rememberBaseline(desktop.Get(), desktopTag);
    require(full && full->status == DesktopTextureBaselineStatus::kQueued && full->copiedBytes == 16384000 && full->copyDispatchMicroseconds, "Full GPU baseline bytes/timing absent");
    std::cout << "baseline_full copied_bytes=" << full->copiedBytes << " cpu_call_us=" << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()
              << " copy_dispatch_and_flush_cpu_us=" << *full->copyDispatchMicroseconds << " physical_gpu_us=unknown completion_not_claimed=1\n";
}

} // namespace
