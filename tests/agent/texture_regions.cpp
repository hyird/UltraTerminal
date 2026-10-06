#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "platform/windows/texture_regions.transport.h"
#include "texture_regions.comparison.fixture.h"

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
agent::DesktopReadbackCompletion scheduledComplete(agent::DesktopTextureRegions& regions) {
    // Bounded scheduled ticks, not a spinning GPU wait. Production checks once
    // at its encoding deadline and sends video instead when still Pending.
    for (unsigned tick = 0; tick < 256; ++tick) {
        Sleep(1);
        auto completed = regions.tryComplete();
        require(completed.has_value(), "Nonblocking Map failed");
        if (completed->status != agent::DesktopReadbackStatus::kPending) {
            return std::move(*completed);
        }
        require(!completed->result && completed->timing.mapMicroseconds && !completed->timing.readbackReadyMicroseconds && !completed->timing.rowCopyMicroseconds,
                "Pending work exposed pixels or fabricated ready/CPU timing");
    }
    throw std::runtime_error("Optional GPU readback did not complete within 256 scheduled ticks");
}
void shortReadback(ID3D11Device* device, ID3D11Texture2D* texture, const std::vector<std::uint8_t>& pixels, unsigned width, unsigned height) {
    agent::DesktopTextureRegions regions(device);
    const std::vector<agent::DesktopReadbackRegion> patches{{128, 64, 128, 64}};
    agent::DesktopReadbackTag tag{7, 11, 13, 17, width, height, -120, 30, 1};
    // No artificial command backlog, synchronous Map, or test-side Flush.
    // The production submit's single asynchronous Flush must provide progress.
    const auto started = std::chrono::steady_clock::now();
    require(regions.submit(texture, tag, patches) == agent::DesktopReadbackSubmit::kSubmitted, "Submit short ROI");
    const auto submitCpu = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    auto probe = regions.tryComplete();
    require(probe && (probe->status == agent::DesktopReadbackStatus::kReady || probe->status == agent::DesktopReadbackStatus::kPending), "Short ROI probe failed");
    const bool firstReady = probe->status == agent::DesktopReadbackStatus::kReady;
    auto completed = firstReady ? std::move(*probe) : scheduledComplete(regions);
    require(completed.status == agent::DesktopReadbackStatus::kReady && completed.result && completed.result->tag == tag && completed.result->pixels == 128 * 64,
            "Short ROI lost source identity or completion opportunity");
    std::vector<std::uint8_t> restored(pixels.size(), 205), expected = restored;
    for (unsigned row = 64; row < 128; ++row) {
        const auto offset = (static_cast<std::size_t>(row) * width + 128) * 4;
        std::memcpy(expected.data() + offset, pixels.data() + offset, 128 * 4);
    }
    require(completed.result->applyTo(restored) && restored == expected, "Short ROI corrupted exact sparse pixels");
    std::cout << "short_roi submit_cpu_us=" << submitCpu << " copy_and_flush_cpu_us=" << completed.timing.copyDispatchMicroseconds.value_or(0)
              << " first_probe_ready=" << firstReady << " observed_ready_us=" << completed.timing.readbackReadyMicroseconds.value_or(0) << '\n';
}
void asynchronousReadback(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::vector<std::uint8_t>& pixels, unsigned width, unsigned height) {
    agent::DesktopTextureRegions regions(device);
    require(regions.tryComplete()->status == agent::DesktopReadbackStatus::kIdle, "New optional slot was not idle");
    agent::DesktopReadbackTag tag{7, 11, 13, 17, width, height, -120, 30, 1};
    std::vector<agent::DesktopReadbackRegion> patches{{128, 64, 128, 64}, {width - 2, height - 2, 2, 2}};
    const auto savedPatches = patches;
    // Real queued GPU/WARP work forces an observable non-ready first check.
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    ComPtr<ID3D11Texture2D> queued;
    require(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &queued)), "Create pending GPU workload");
    for (unsigned command = 0; command < 512; ++command) {
        context->CopyResource(queued.Get(), texture);
    }
    require(regions.submit(texture, tag, patches) == agent::DesktopReadbackSubmit::kSubmitted, "Submit optional ROI");
    require(regions.submit(texture, tag, patches) == agent::DesktopReadbackSubmit::kBusy, "Submitted slot was reused");
    patches.clear(); // submit owns the descriptor; it must not borrow this vector.
    auto pending = regions.tryComplete();
    require(pending && (pending->status == agent::DesktopReadbackStatus::kPending || pending->status == agent::DesktopReadbackStatus::kReady),
            "Queued real GPU readback did not report Pending/Ready");
    const bool firstPending = pending->status == agent::DesktopReadbackStatus::kPending;
    if (firstPending) {
        require(!pending->result && pending->timing.mapMicroseconds && !pending->timing.readbackReadyMicroseconds,
                "Pending work exposed pixels or fabricated readiness");
    }
    std::cout << "queued_roi first_pending=" << firstPending << '\n';
    std::vector<std::uint8_t> overwritten(pixels.size(), 37);
    context->UpdateSubresource(texture, 0, nullptr, overwritten.data(), width * 4, 0);
    auto completed = firstPending ? scheduledComplete(regions) : std::move(*pending);
    require(completed.status == agent::DesktopReadbackStatus::kReady && completed.result && completed.result->tag == tag &&
                completed.timing.copyDispatchMicroseconds && completed.timing.mapMicroseconds && completed.timing.rowCopyMicroseconds && completed.timing.readbackReadyMicroseconds,
            "Ready result lost identity or stage presence");
    std::vector<std::uint8_t> copied(pixels.size(), 205), expected = copied;
    for (const auto& patch : savedPatches) {
        for (unsigned row = 0; row < patch.height; ++row) {
            const auto offset = (static_cast<std::size_t>(patch.y + row) * width + patch.x) * 4;
            std::memcpy(expected.data() + offset, pixels.data() + offset, patch.width * 4);
        }
    }
    require(completed.result->applyTo(copied) && copied == expected, "Copy did not freeze source pixels or sparse rows corrupted untouched storage");
    auto malformed = *completed.result;
    malformed.regions.back().x = width;
    require(!malformed.applyTo(copied) && copied == expected, "Invalid result partially wrote destination");
    require(!completed.result->applyTo(std::span(copied).first(copied.size() - 1)), "Wrong destination extent accepted");
    malformed = *completed.result;
    malformed.tag.width = malformed.tag.height = 1u << 31;
    require(!malformed.applyTo({}), "Overflowing plain result geometry accepted");
    require(regions.tryComplete()->status == agent::DesktopReadbackStatus::kIdle, "Completed result delivered twice");
    // A different texture of identical dimensions must have its own source ID.
    D3D11_SUBRESOURCE_DATA secondInitial{overwritten.data(), width * 4, 0};
    ComPtr<ID3D11Texture2D> second;
    require(SUCCEEDED(device->CreateTexture2D(&description, &secondInitial, &second)), "Create same-size second source");
    ++tag.sourceSerial;
    require(regions.submit(second.Get(), tag, savedPatches) == agent::DesktopReadbackSubmit::kSubmitted, "Submit second source");
    auto secondResult = scheduledComplete(regions);
    require(secondResult.result && secondResult.result->tag == tag && secondResult.result->bgra != completed.result->bgra,
            "Same-size second source inherited old snapshot");
    require(regions.submit(second.Get(), tag, savedPatches) == agent::DesktopReadbackSubmit::kSubmitted, "Submit retired work");
    regions.retire();
    regions.retire();
    require(regions.submit(second.Get(), tag, savedPatches) == agent::DesktopReadbackSubmit::kBusy, "Retirement prematurely released physical slot");
    auto retired = scheduledComplete(regions);
    require(retired.status == agent::DesktopReadbackStatus::kRetired && !retired.result && retired.timing.readbackReadyMicroseconds && !retired.timing.rowCopyMicroseconds,
            "Retired readback delivered pixels or ran CPU row copy");
    tag.captureEpoch++;
    require(regions.submit(second.Get(), tag, savedPatches) == agent::DesktopReadbackSubmit::kSubmitted && scheduledComplete(regions).result->tag == tag,
            "Physically completed retired slot could not be reused");
    {
        agent::DesktopTextureRegions released(device);
        require(released.submit(second.Get(), tag, savedPatches) == agent::DesktopReadbackSubmit::kSubmitted, "Submit for in-flight destruction");
        released.retire(); // Destruction releases COM owners, never waits/maps.
    }
    const std::vector<agent::DesktopReadbackRegion> invalid{{width, 0, 1, 1}};
    require(!regions.submit(second.Get(), tag, invalid) && regions.tryComplete()->status == agent::DesktopReadbackStatus::kIdle, "Malformed ROI consumed slot");
    context->UpdateSubresource(texture, 0, nullptr, pixels.data(), width * 4, 0);
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
        fusedComparison(device.Get(), context.Get(), width, height);
        shortReadback(device.Get(), texture.Get(), pixels, width, height);
        asynchronousReadback(device.Get(), context.Get(), texture.Get(), pixels, width, height);
        agent::DesktopTextureRegions unavailable(nullptr);
        agent::DesktopReadbackTag unavailableTag{1, 1, 1, 1, width, height};
        const std::vector<agent::DesktopReadbackRegion> unavailablePatch{{0, 0, 1, 1}};
        require(unavailable.submit(texture.Get(), unavailableTag, unavailablePatch) == agent::DesktopReadbackSubmit::kUnavailable &&
                    unavailable.tryComplete()->status == agent::DesktopReadbackStatus::kIdle,
                "Absent optional device did not fall back without work");
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
        // Entire image assembled from all edge tiles, not sparse bytes falsely
        // advertised as a full CPU baseline.
        std::vector<agent::DesktopReadbackRegion> whole;
        for (unsigned y = 0; y < 130; y += 64) {
            for (unsigned x = 0; x < 258; x += 128) {
                whole.push_back({x, y, std::min(128u, 258 - x), std::min(64u, 130 - y)});
            }
        }
        agent::DesktopReadbackTag edgeTag{1, 2, 3, 4, 258, 130};
        require(regions.submit(edgeTexture.Get(), edgeTag, whole) == agent::DesktopReadbackSubmit::kSubmitted, "Submit complete tiled readback");
        auto full = scheduledComplete(regions);
        require(full.result && full.result->applyTo(edgeCopied) && edgeCopied == edgePixels, "Full tiled pixels or edge rows disagreed");
        changes = regions.changes(edgeTexture.Get());
        require(changes && std::count(changes->begin(), changes->end(), 1u) == 1, "Optional readback advanced submitted RGB baseline");
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
        const agent::DesktopReadbackTag olderTag{5, 6, 7, 8, width, height};
        require(older.rememberBaseline(olderTexture.Get(), olderTag)->status == agent::DesktopTextureBaselineStatus::kUnavailable &&
                    older.submitComparison(olderTexture.Get(), olderTag, olderTag, unavailablePatch) == agent::DesktopReadbackSubmit::kUnavailable,
                "Missing optional compute returned false equality proof");
        require(unavailable.rememberBaseline(texture.Get(), olderTag)->status == agent::DesktopTextureBaselineStatus::kUnavailable &&
                    unavailable.submitComparison(texture.Get(), olderTag, olderTag, unavailablePatch) == agent::DesktopReadbackSubmit::kUnavailable,
                "Null device returned false baseline proof");
        require(older.submit(olderTexture.Get(), olderTag, unavailablePatch) == agent::DesktopReadbackSubmit::kSubmitted && scheduledComplete(older).result.has_value(),
                "Nonblocking ROI required optional compiler/compute");
        std::cout << (hardware ? "Hardware" : "WARP") << " exact GPU tile changes, async Pending/Busy/retirement/source identity, complete/sparse atlas pixels, alpha, committed baseline and optional fallback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
