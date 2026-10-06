#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "capture.fixture.h"
#include "features/desktop/software_video.transport.h"
#include "platform/windows/capture.transport.h"

namespace {
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
const char* describeColors(const std::array<COLORREF, 4>& samples) {
    constexpr std::array<COLORREF, 4> colors{RGB(208, 32, 32), RGB(32, 208, 32), RGB(32, 32, 208), RGB(208, 208, 32)};
    const auto matches = [](COLORREF left, COLORREF right) {
        return std::abs(GetRValue(left) - GetRValue(right)) < 35 &&
               std::abs(GetGValue(left) - GetGValue(right)) < 35 &&
               std::abs(GetBValue(left) - GetBValue(right)) < 35;
    };
    if (std::all_of(samples.begin(), samples.end(), [&](COLORREF value) { return matches(value, RGB(0, 0, 0)); })) {
        return "black";
    }
    for (int direction : {1, -1}) {
        bool correct = true;
        for (int index = 0; index < 4; ++index) {
            if (!matches(samples[index], colors[direction == 1 ? index : 3 - index])) {
                correct = false;
            }
        }
        if (correct) {
            return "pattern";
        }
    }
    return "other";
}
void listOutputs() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("Cannot enumerate DXGI adapters");
    }
    const auto utf8 = [](const wchar_t* wide) {
        char text[512]{};
        const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, static_cast<int>(sizeof(text)), nullptr, nullptr);
        return length ? std::string(text, length - 1) : std::string("unknown");
    };
    unsigned attached = 0;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto status = factory->EnumAdapters1(i, &adapter);
        if (status == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(status)) {
            throw std::runtime_error("Cannot query DXGI adapter");
        }
        DXGI_ADAPTER_DESC1 adapterInfo{};
        if (FAILED(adapter->GetDesc1(&adapterInfo))) {
            throw std::runtime_error("Cannot describe DXGI adapter");
        }
        for (UINT j = 0;; ++j) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            const auto outputStatus = adapter->EnumOutputs(j, &output);
            if (outputStatus == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputStatus)) {
                throw std::runtime_error("Cannot query DXGI output");
            }
            DXGI_OUTPUT_DESC description{};
            if (FAILED(output->GetDesc(&description))) {
                throw std::runtime_error("Cannot describe DXGI output");
            }
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            const auto primary = description.Monitor && GetMonitorInfoW(description.Monitor, &monitor) && (monitor.dwFlags & MONITORINFOF_PRIMARY);
            std::cout << "adapter=" << i << " output=" << j << " attached_index=";
            if (description.AttachedToDesktop) {
                std::cout << attached++;
            } else {
                std::cout << "none";
            }
            std::cout << " adapter_name=" << utf8(adapterInfo.Description) << " display=" << utf8(description.DeviceName)
                      << " attached=" << (description.AttachedToDesktop ? "yes" : "no")
                      << " primary=" << (primary ? "yes" : "no")
                      << " bounds=" << description.DesktopCoordinates.left << ',' << description.DesktopCoordinates.top
                      << ',' << description.DesktopCoordinates.right << ',' << description.DesktopCoordinates.bottom << '\n';
        }
    }
}
void diagnosePixels() {
    const auto window = FindWindowW(L"UltraterimalCaptureVerification", nullptr);
    if (!window) {
        std::cout << "observer_window=missing\n";
        return;
    }
    const auto visible = IsWindowVisible(window) != FALSE;
    std::array<COLORREF, 4> windowPixels{};
    const auto dc = GetDC(window);
    for (int i = 0; i < 4; ++i) {
        windowPixels[i] = dc ? GetPixel(dc, 32 + i * 64, 64) : CLR_INVALID;
    }
    if (dc) {
        ReleaseDC(window, dc);
    }
    RECT windowBounds{};
    if (!GetWindowRect(window, &windowBounds)) {
        throw std::runtime_error("Cannot locate test window");
    }
    std::array<COLORREF, 4> screenPixels{};
    const auto screen = GetDC(nullptr);
    for (int i = 0; i < 4; ++i) {
        screenPixels[i] = screen ? GetPixel(screen, windowBounds.left + 32 + i * 64, windowBounds.top + 64) : CLR_INVALID;
    }
    if (screen) {
        ReleaseDC(nullptr, screen);
    }
    DWORD cloaked = 0;
    const auto cloakKnown = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
    const auto overlaid = GetAncestor(WindowFromPoint(POINT{windowBounds.left + 32, windowBounds.top + 64}), GA_ROOT);
    char visibleClass[256]{};
    if (overlaid) {
        GetClassNameA(overlaid, visibleClass, sizeof(visibleClass));
    }
    std::cout << "observer_window=" << (visible ? "visible" : "hidden") << " observer_dc=" << describeColors(windowPixels)
              << " screen_dc=" << describeColors(screenPixels) << " cloak=" << (cloakKnown ? (cloaked ? "yes" : "no") : "unknown")
              << " hit_observer=" << (overlaid == window ? "yes" : "no") << " hit_class=" << (overlaid ? visibleClass : "none");
    ultraterimal::agent::DesktopCapture capture;
    for (int attempt = 0; attempt < 30; ++attempt) {
        auto frame = capture.nextFrame(std::chrono::milliseconds(100));
        if (!frame || frame->bgra.empty()) {
            continue;
        }
        std::array<COLORREF, 4> pixels{};
        if (frame->width < 320 || frame->height < 100) {
            throw std::runtime_error("Desktop smaller than test pattern");
        }
        for (int i = 0; i < 4; ++i) {
            const auto offset = (static_cast<std::size_t>(96) * frame->width + 64 + i * 64) * 4;
            const auto* sample = frame->bgra.data() + offset;
            pixels[i] = RGB(sample[2], sample[1], sample[0]);
        }
        std::cout << " dxgi=" << describeColors(pixels) << " present_tick=" << (frame->captureTicks ? "nonzero" : "zero") << '\n';
        return;
    }
    std::cout << " dxgi=timeout\n";
}
void expectInactiveDesktopRejected() {
    // Never switch the user's visible desktop. Only this fresh worker changes desktops.
    bool rejected = false;
    std::thread worker([&] {
        const auto original = GetThreadDesktop(GetCurrentThreadId());
        const auto desktop = CreateDesktopW(L"UltraterimalCaptureIsolationTest", nullptr, nullptr, 0,
                                            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS, nullptr);
        if (!desktop) {
            return;
        }
        if (SetThreadDesktop(desktop)) {
            try {
                ultraterimal::agent::DesktopCapture capture;
            } catch (const std::runtime_error& error) {
                rejected = std::string_view(error.what()) == "Interactive desktop changed or locked";
            }
            SetThreadDesktop(original);
        }
        CloseDesktop(desktop);
    });
    worker.join();
    if (!rejected) {
        throw std::runtime_error("Capture did not reject an inactive desktop");
    }
}
std::uintptr_t expectColor(ultraterimal::agent::DesktopCapture& capture, std::vector<std::uint8_t>& pixels, COLORREF color, std::uintptr_t allocation = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto previousData = pixels.data();
        const auto previousSize = pixels.size();
        auto frame = capture.nextFrame(std::chrono::milliseconds(100), &pixels, nullptr, !pixels.empty());
        if ((!frame || frame->bgra.empty()) && (pixels.data() != previousData || pixels.size() != previousSize)) {
            throw std::runtime_error("Unchanged snapshot lost retained pixels");
        }
        if (!frame) {
            continue;
        } // Unchanged desktop legitimately returns no frame.
        if (frame->bgra.empty()) {
            if (frame->captureTicks != 0) {
                throw std::runtime_error("Desktop pixel update omitted its pixels");
            }
            continue; // Cursor-only updates retain the previously captured pixels.
        }
        require(frame->captureEpoch && frame->sourceSerial && frame->stageTiming.acquireWaitMicroseconds && frame->stageTiming.mapMicroseconds && frame->stageTiming.rowCopyMicroseconds,
                "Complete snapshot omitted identity or production duration presence");
        if (!frame->width || !frame->height || frame->bgra.size() != static_cast<std::size_t>(frame->width) * frame->height * 4) {
            throw std::runtime_error("Invalid captured pixel storage");
        }
        const auto storage = reinterpret_cast<std::uintptr_t>(frame->bgra.data());
        if (allocation && allocation != storage) {
            throw std::runtime_error("Capture did not reuse returned pixel storage");
        }
        std::size_t matching = 0;
        for (std::size_t i = 0; i < frame->bgra.size(); i += 4) {
            if (frame->bgra[i] == GetBValue(color) && frame->bgra[i + 1] == GetGValue(color) && frame->bgra[i + 2] == GetRValue(color)) {
                ++matching;
            }
        }
        if (matching >= 32 * 32) {
            ultraterimal::agent::SoftwareVideoEncoder encoder({frame->width, frame->height, 8000000, 30});
            const auto encoded = encoder.encode(frame->bgra, static_cast<std::size_t>(frame->width) * 4, 0);
            if (!encoded || !encoded->keyFrame || encoded->annexB.empty()) {
                throw std::runtime_error("Actual desktop pixels could not be software encoded");
            }
            std::cout << "Verified compositor pixels at " << frame->width << "x" << frame->height << '\n';
            pixels = std::move(frame->bgra);
            return storage;
        }
        pixels = std::move(frame->bgra);
    }
    throw std::runtime_error("Captured frames did not contain test surface pixels");
}
ultraterimal::agent::DesktopReadbackCompletion scheduledComplete(ultraterimal::agent::DesktopCapture& capture, const ultraterimal::agent::DesktopReadbackTag& tag) {
    for (unsigned tick = 0; tick < 256; ++tick) {
        Sleep(1); // One check per bounded scheduling tick; never spin or Flush.
        auto completed = capture.tryCompleteTextureRegions(tag);
        require(completed.has_value(), "Capture optional Map failed");
        if (completed->status != ultraterimal::agent::DesktopReadbackStatus::kPending) {
            return std::move(*completed);
        }
    }
    throw std::runtime_error("Capture optional slot did not finish within bounded ticks");
}
void verifyComparisonContextSwitch() {
    using namespace ultraterimal::agent;
    std::exception_ptr failure;
    std::thread worker([&] {
        const auto original = GetThreadDesktop(GetCurrentThreadId());
        const auto inactive = CreateDesktopW(L"UltraterimalComparisonIsolationTest", nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS, nullptr);
        try {
            require(inactive != nullptr, "Create comparison isolation desktop");
            DesktopCapture capture;
            auto frame = capture.nextFrame(std::chrono::milliseconds(100), nullptr, nullptr, false, false);
            require(frame.has_value(), "Context switch fixture has no initial source");
            const auto tag = frame->readbackTag(7, 9);
            require(capture.rememberTextureBaseline(tag)->status == DesktopTextureBaselineStatus::kQueued, "Remember context switch baseline");
            require(SetThreadDesktop(inactive) != FALSE, "Set isolated worker desktop");
            const std::vector<DesktopReadbackRegion> areas{{0, 0, 1, 1}};
            bool rejected = false;
            try {
                static_cast<void>(capture.submitTextureComparison(tag, tag, areas));
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            require(SetThreadDesktop(original) != FALSE, "Restore isolated worker desktop");
            require(rejected && capture.submitTextureComparison(tag, tag, areas) == DesktopReadbackSubmit::kUnavailable, "Context switch failed to invalidate comparison key");
        } catch (...) {
            failure = std::current_exception();
        }
        SetThreadDesktop(original);
        if (inactive) {
            CloseDesktop(inactive);
        }
    });
    worker.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    std::cout << "Verified comparison key invalidation after isolated worker context switch (no visible desktop switch)\n";
}
void verifyOptionalCapture(ultraterimal::test::TestSurface& surface) {
    using namespace ultraterimal::agent;
    std::uint64_t previousEpoch = 0;
    {
        DesktopCapture capture;
        const std::vector<DesktopReadbackRegion> initialArea{{0, 0, 1, 1}};
        require(capture.submitTextureRegions({}, initialArea) == DesktopReadbackSubmit::kUnavailable, "Unretained capture did not fall back");
        const auto snapshot = [&](COLORREF color) {
            surface.paint(color);
            for (unsigned attempt = 0; attempt < 30; ++attempt) {
                auto frame = capture.nextFrame(std::chrono::milliseconds(100), nullptr, nullptr, false, true, true);
                if (!frame || frame->bgra.empty()) {
                    continue;
                }
                const auto point = surface.cursorPosition();
                const auto x = point.x - frame->desktopLeft, y = point.y - frame->desktopTop;
                require(x >= 0 && y >= 0 && static_cast<unsigned>(x) < frame->width && static_cast<unsigned>(y) < frame->height, "Surface outside captured geometry");
                const auto offset = (static_cast<std::size_t>(y) * frame->width + x) * 4;
                if (frame->bgra[offset] == GetBValue(color) && frame->bgra[offset + 1] == GetGValue(color) && frame->bgra[offset + 2] == GetRValue(color)) {
                    return std::move(*frame);
                }
            }
            throw std::runtime_error("No retained compositor snapshot of test color");
        };
        auto frame = snapshot(RGB(237, 19, 71));
        const auto position = surface.cursorPosition();
        const std::vector<DesktopReadbackRegion> patches{{static_cast<unsigned>(position.x - frame.desktopLeft), static_cast<unsigned>(position.y - frame.desktopTop), 2, 2}};
        auto tag = frame.readbackTag(3, 5);
        const auto baseline = capture.rememberTextureBaseline(tag);
        require(baseline && baseline->status == DesktopTextureBaselineStatus::kQueued && baseline->copyDispatchMicroseconds, "Capture independent baseline unavailable");
        require(capture.submitTextureComparison(tag, tag, patches) == DesktopReadbackSubmit::kSubmitted && capture.submitTextureRegions(tag, patches) == DesktopReadbackSubmit::kBusy,
                "Capture comparison/ordinary slots were independent");
        const auto fused = scheduledComplete(capture, tag);
        require(fused.result && fused.result->comparison && fused.result->comparison->baselineTag == tag && fused.result->comparison->changed == std::vector<std::uint32_t>({0}), "Static capture equality incorrect");
        std::cout << "capture_baseline bytes=" << baseline->copiedBytes << " copy_dispatch_cpu_us=" << *baseline->copyDispatchMicroseconds
                  << " fused_observed_ready_us=" << fused.timing.readbackReadyMicroseconds.value_or(0) << '\n';
        const auto original = frame.bgra;
        capture.readTexture(frame);
        require(frame.bgra == original && frame.stageTiming.mapMicroseconds && frame.stageTiming.rowCopyMicroseconds, "Full retained readback changed pixels or omitted duration");
        require(capture.submitTextureRegions(tag, patches) == DesktopReadbackSubmit::kSubmitted, "Capture optional submit");
        auto completed = scheduledComplete(capture, tag);
        std::vector<std::uint8_t> sparse(original.size(), 205);
        require(completed.result && completed.result->tag == tag && completed.result->applyTo(sparse), "Capture adapter lost result identity");
        for (std::size_t index = 0; index < sparse.size(); ++index) {
            const auto pixel = index / 4;
            const auto x = pixel % frame.width, y = pixel / frame.width;
            const auto& area = patches.front();
            const bool inside = x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height;
            require(sparse[index] == (inside ? original[index] : 205), "Capture sparse ROI corrupted full-buffer integrity");
        }
        // All portions of the tag are barriers, including same-size geometry and
        // base/generation changes that do not replace the physical source texture.
        for (unsigned field = 0; field < 9; ++field) {
            auto stale = tag;
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
            require(capture.submitTextureComparison(tag, stale, patches) == DesktopReadbackSubmit::kUnavailable, "Capture accepted wrong comparison baseline");
            require(capture.submitTextureComparison(tag, tag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit fused tag barrier test");
            const auto staleFused = scheduledComplete(capture, stale);
            require(staleFused.status == DesktopReadbackStatus::kRetired && !staleFused.result, "Mismatched fused output escaped retirement");
            require(capture.submitTextureRegions(tag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit tag barrier test");
            auto retired = scheduledComplete(capture, stale);
            require(retired.status == DesktopReadbackStatus::kRetired && !retired.result, "Mismatched capture tag escaped retirement");
        }
        require(capture.submitTextureRegions(tag, patches) == DesktopReadbackSubmit::kSubmitted, "Submit before source overwrite");
        capture.retireTextureRegions();
        require(capture.submitTextureRegions(tag, patches) == DesktopReadbackSubmit::kBusy, "Capture retirement returned premature credit");
        auto next = snapshot(RGB(17, 211, 139));
        require(next.captureEpoch == frame.captureEpoch && next.sourceSerial > frame.sourceSerial, "Same-size new capture reused snapshot identity");
        auto retired = scheduledComplete(capture, tag);
        require(retired.status == DesktopReadbackStatus::kRetired && !retired.result, "Overwrite allowed old result to commit");
        require(!capture.submitTextureRegions(tag, patches), "Old same-size source accepted for new Copy");
        bool rejected = false;
        try {
            capture.readTexture(frame);
        } catch (const CaptureReset&) {
            rejected = true;
        }
        require(rejected, "Synchronous read silently substituted a newer same-size snapshot");
        const auto nextComparisonTag = next.readbackTag(tag.generation, tag.composedBase);
        require(capture.rememberTextureBaseline(tag)->status == DesktopTextureBaselineStatus::kUnavailable && capture.submitTextureComparison(tag, tag, patches) == DesktopReadbackSubmit::kUnavailable,
                "Overwritten source accepted for baseline/copy");
        require(capture.submitTextureComparison(nextComparisonTag, tag, patches) == DesktopReadbackSubmit::kSubmitted, "Independent baseline did not survive acquire overwrite");
        const auto changed = scheduledComplete(capture, nextComparisonTag);
        require(changed.result && changed.result->comparison && changed.result->comparison->baselineTag == tag && changed.result->comparison->changed == std::vector<std::uint32_t>({1}),
                "Acquire advanced baseline or lost changed capture ROI");
        capture.clearTextureBaseline();
        require(capture.submitTextureComparison(nextComparisonTag, tag, patches) == DesktopReadbackSubmit::kUnavailable, "Capture clear retained baseline key");
        auto nextTag = next.readbackTag(3, 6);
        require(capture.rememberTextureBaseline(nextTag)->status == DesktopTextureBaselineStatus::kQueued, "Explicit next baseline failed");
        require(capture.submitTextureRegions(nextTag, patches) == DesktopReadbackSubmit::kSubmitted && scheduledComplete(capture, nextTag).result.has_value(), "Completed retired slot could not serve new snapshot");
        previousEpoch = next.captureEpoch;
    }
    DesktopCapture another;
    const auto other = another.nextFrame(std::chrono::milliseconds(100));
    require(other && other->captureEpoch != previousEpoch, "Capture recreation reused epoch");
    const std::vector<DesktopReadbackRegion> area{{0, 0, 1, 1}};
    require(another.submitTextureComparison(other->readbackTag(3, 6), {}, area) == DesktopReadbackSubmit::kUnavailable, "Recreated capture carried comparison baseline");
    std::cout << "Verified optional capture identity barriers, sparse pixels, retirement and production timings\n";
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--diagnose") {
            diagnosePixels();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--outputs") {
            listOutputs();
            return 0;
        }
        if (argc != 1) {
            return 2;
        }
        expectInactiveDesktopRejected();
        ultraterimal::test::TestSurface surface;
        verifyOptionalCapture(surface);
        verifyComparisonContextSwitch();
        {
            ultraterimal::agent::DesktopCapture capture;
            std::vector<std::uint8_t> pixels;
            surface.paint(RGB(237, 19, 71));
            const auto allocation = expectColor(capture, pixels, RGB(237, 19, 71));
            const auto original = pixels;
            surface.paint(RGB(17, 211, 139));
            bool deferred = false;
            for (int attempt = 0; attempt < 30; ++attempt) {
                ultraterimal::agent::CaptureTiming timing;
                const auto update = capture.nextFrame(std::chrono::milliseconds(16), &pixels, &timing, true, false);
                require(pixels == original && timing.mapMilliseconds == 0 && timing.rowCopyMilliseconds == 0 &&
                            timing.stages.acquireWaitMicroseconds && !timing.stages.mapMicroseconds && !timing.stages.rowCopyMicroseconds,
                        "Deferred capture performed CPU readback, fabricated stage presence or consumed the previous baseline");
                if (update && update->captureTicks) {
                    require(update->bgra.empty(), "Closed receiver window materialized pixels");
                    deferred = true;
                    break;
                }
            }
            require(deferred, "DXGI did not retain a deferred desktop update");
            expectColor(capture, pixels, RGB(17, 211, 139), allocation);
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            ultraterimal::agent::DesktopCapture capture;
            std::vector<std::uint8_t> pixels;
            surface.paint(RGB(237, 19, 71));
            const auto allocation = expectColor(capture, pixels, RGB(237, 19, 71));
            surface.paint(RGB(17, 211, 139));
            expectColor(capture, pixels, RGB(17, 211, 139), allocation);
        }
        // A new desktop connection must capture an already-static surface;
        // repainting after each DuplicateOutput would hide an invalid initial
        // snapshot retained until the next real compositor update.
        surface.paint(RGB(17, 211, 139));
        for (int attempt = 0; attempt < 12; ++attempt) {
            ultraterimal::agent::DesktopCapture capture;
            std::vector<std::uint8_t> pixels;
            expectColor(capture, pixels, RGB(17, 211, 139));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
