#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "capture.fixture.h"
#include "features/desktop/software_video.transport.h"
#include "platform/windows/capture.transport.h"

namespace {
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
