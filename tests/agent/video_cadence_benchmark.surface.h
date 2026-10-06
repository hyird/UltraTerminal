#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "video_cadence_benchmark.samples.h"

namespace ultraterimal::test::cadence {
struct PaintCallbackCounter {
    std::atomic<std::uint64_t> count{0};
    std::atomic<bool> attached{false};
};
// Created/destroyed on the existing painter thread, after/before its surface.
// No USERDATA mutation, UI send, extra loop or change to the original drawing.
class PaintCallbackObserver {
  public:
    PaintCallbackObserver(const wchar_t* className, PaintCallbackCounter& counter) : counter_(counter) {
        window_ = FindWindowW(className, nullptr);
        if (current_ || !window_ || GetWindowThreadProcessId(window_, nullptr) != GetCurrentThreadId()) {
            throw std::runtime_error("Cannot observe painter window on its owning thread");
        }
        SetLastError(ERROR_SUCCESS);
        const auto previous = SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(windowProc));
        if (!previous) {
            throw std::runtime_error("Cannot attach WM_PAINT callback observer");
        }
        previous_ = reinterpret_cast<WNDPROC>(previous);
        current_ = this;
        counter_.attached.store(true);
    }
    ~PaintCallbackObserver() {
        counter_.attached.store(false);
        SetWindowLongPtrW(window_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_));
        // Even if OS restoration fails, the static trampoline keeps no dangling
        // pointer: subsequent teardown messages use DefWindowProc, not this.
        current_ = nullptr;
    }
    PaintCallbackObserver(const PaintCallbackObserver&) = delete;
    PaintCallbackObserver& operator=(const PaintCallbackObserver&) = delete;

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = current_;
        if (!self || self->window_ != window) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == WM_PAINT) {
            self->counter_.count.fetch_add(1); // Real callback entry, not paint() calls.
        }
        return CallWindowProcW(self->previous_, window, message, wparam, lparam);
    }
    inline static thread_local PaintCallbackObserver* current_ = nullptr;
    PaintCallbackCounter& counter_;
    HWND window_ = nullptr;
    WNDPROC previous_ = nullptr;
};
inline std::optional<std::uint64_t> paintQpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) && value.QuadPart >= 0 ? std::optional(static_cast<std::uint64_t>(value.QuadPart)) : std::nullopt;
}
inline std::optional<std::uint64_t> paintQpcFrequency() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? std::optional(static_cast<std::uint64_t>(value.QuadPart)) : std::nullopt;
}
inline PaintRead readPaintCounter(const PaintCallbackCounter& counter) noexcept {
    PaintRead result;
    result.qpcBefore = paintQpc();
    if (counter.attached.load()) {
        result.count = counter.count.load();
    }
    if (!counter.attached.load()) {
        result.count.reset();
    }
    result.qpcAfter = paintQpc();
    return result;
}
struct SurfaceEvidence {
    RECT rectangle{}, desktop{};
    std::uint64_t wmPaints = 0;
    bool visible = false, centerOnTop = false;
};
inline RECT displayBounds() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("Cannot enumerate benchmark display");
    }
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) != S_OK) {
            break;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) != S_OK) {
                break;
            }
            DXGI_OUTPUT_DESC description{};
            if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop) {
                return description.DesktopCoordinates;
            }
        }
    }
    throw std::runtime_error("No interactive benchmark display");
}
inline void inspectSurface(HWND window, SurfaceEvidence& evidence) {
    if (!window || !GetWindowRect(window, &evidence.rectangle)) {
        throw std::runtime_error("Cannot read actual benchmark window rectangle");
    }
    DWORD cloaked = 0;
    evidence.visible = IsWindowVisible(window) && SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked;
    POINT point{(evidence.rectangle.left + evidence.rectangle.right) / 2, (evidence.rectangle.top + evidence.rectangle.bottom) / 2};
    evidence.centerOnTop = GetAncestor(WindowFromPoint(point), GA_ROOT) == window;
}
// Only the benchmark's static-detail path uses this rectangular, optionally
// output-covering window. It neither resizes the OS nor dismisses other UI.
class StaticDetailSurface {
  public:
    StaticDetailSurface(int size, bool fullDesktop, SurfaceEvidence& evidence) : evidence_(evidence) {
        if (!SetThreadExecutionState(ES_DISPLAY_REQUIRED)) {
            throw std::runtime_error("Cannot wake benchmark display");
        }
        evidence_.desktop = displayBounds();
        auto rectangle = evidence_.desktop;
        if (!fullDesktop) {
            if (size < 64 || size > rectangle.right - rectangle.left - 64 || size > rectangle.bottom - rectangle.top - 64) {
                throw std::invalid_argument("Static detail surface does not fit display");
            }
            rectangle.left += 32;
            rectangle.top += 32;
            rectangle.right = rectangle.left + size;
            rectangle.bottom = rectangle.top + size;
        }
        width_ = rectangle.right - rectangle.left;
        height_ = rectangle.bottom - rectangle.top;
        pixels_.resize(static_cast<std::size_t>(width_) * height_);
        // Same odd 1px/high-palette detail pattern as TestSurface::paintDetail(0).
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                auto pixel = (100u << 16) | (static_cast<unsigned>(y % 64) << 8) | static_cast<unsigned>(x % 128);
                if (x % 128 == 3 || x % 128 == 127 || y % 64 == 5 || y % 64 == 63) {
                    pixel = (16u << 16) | (50u << 8) | 170u;
                } else if (x % 128 == 4 || y % 64 == 6) {
                    pixel = (55u << 16) | (75u << 8) | 120u;
                }
                pixels_[static_cast<std::size_t>(y) * width_ + x] = pixel;
            }
        }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        if (!RegisterClassW(&windowClass)) {
            throw std::runtime_error("Cannot register static detail test window");
        }
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP, rectangle.left, rectangle.top, width_, height_, nullptr, nullptr, windowClass.hInstance, this);
        if (!window_) {
            UnregisterClassW(kClassName, windowClass.hInstance);
            throw std::runtime_error("Cannot create static detail test window");
        }
    }
    ~StaticDetailSurface() {
        DestroyWindow(window_);
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
    }
    StaticDetailSurface(const StaticDetailSurface&) = delete;
    StaticDetailSurface& operator=(const StaticDetailSurface&) = delete;
    void paintOnce() {
        if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) || !InvalidateRect(window_, nullptr, FALSE)) {
            throw std::runtime_error("Cannot show static detail surface");
        }
        UpdateWindow(window_);
        if (renderFailed_ || !evidence_.wmPaints || FAILED(DwmFlush())) {
            throw std::runtime_error("Cannot render static detail WM_PAINT");
        }
        inspectSurface(window_, evidence_);
    }
    void pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (renderFailed_) {
            throw std::runtime_error("Cannot repaint static detail surface");
        }
        inspectSurface(window_, evidence_);
    }

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<StaticDetailSurface*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            BITMAPINFO bitmap{};
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = self->width_;
            bitmap.bmiHeader.biHeight = -self->height_;
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            self->renderFailed_ |= SetDIBitsToDevice(dc, 0, 0, self->width_, self->height_, 0, 0, 0, self->height_, self->pixels_.data(), &bitmap, DIB_RGB_COLORS) != static_cast<int>(self->height_);
            ++self->evidence_.wmPaints;
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    inline static constexpr wchar_t kClassName[] = L"UltraterimalCadenceStaticDetailTest";
    SurfaceEvidence& evidence_;
    HWND window_ = nullptr;
    int width_ = 0, height_ = 0;
    std::vector<std::uint32_t> pixels_;
    bool renderFailed_ = false;
};
} // namespace ultraterimal::test::cadence
