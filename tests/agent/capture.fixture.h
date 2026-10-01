#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
namespace ultraterimal::test {
class TestSurface {
  public:
    explicit TestSurface(int size = 64) : size_(size) {
        // Reset the display idle timer so this test also runs after monitor power saving.
        // No persistent power policy or execution-state requirement is installed.
        if (!SetThreadExecutionState(ES_DISPLAY_REQUIRED)) {
            throw std::runtime_error("Cannot wake test display");
        }
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            throw std::runtime_error("Cannot enumerate test display");
        }
        RECT bounds{};
        bool found = false;
        for (UINT i = 0; !found; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) != S_OK) {
                break;
            }
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(j, &output) != S_OK) {
                    break;
                }
                DXGI_OUTPUT_DESC description{};
                if (SUCCEEDED(output->GetDesc(&description)) && description.AttachedToDesktop) {
                    bounds = description.DesktopCoordinates;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            throw std::runtime_error("No interactive display for capture test");
        }
        if (size < 64 || size > bounds.right - bounds.left - 64 || size > bounds.bottom - bounds.top - 64) {
            throw std::invalid_argument("Test surface does not fit interactive display " + std::to_string(bounds.right - bounds.left) + "x" + std::to_string(bounds.bottom - bounds.top));
        }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"UltraterimalCaptureTest";
        if (!RegisterClassW(&windowClass)) {
            throw std::runtime_error("Cannot register test window");
        }
        window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  windowClass.lpszClassName, L"", WS_POPUP,
                                  bounds.left + 32, bounds.top + 32, size, size,
                                  nullptr, nullptr, windowClass.hInstance, this);
        if (!window_) {
            UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
            throw std::runtime_error("Cannot create test window");
        }
        SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    ~TestSurface() {
        DestroyWindow(window_);
        UnregisterClassW(L"UltraterimalCaptureTest", GetModuleHandleW(nullptr));
    }
    TestSurface(const TestSurface&) = delete;
    TestSurface& operator=(const TestSurface&) = delete;
    bool visible() const {
        DWORD cloaked = 0;
        return IsWindowVisible(window_) && SUCCEEDED(DwmGetWindowAttribute(window_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && !cloaked;
    }
    bool centerOnTop() const {
        POINT point{32, 32};
        return ClientToScreen(window_, &point) && GetAncestor(WindowFromPoint(point), GA_ROOT) == window_;
    }
    POINT stripPosition() const {
        POINT position{8, 16};
        if (!ClientToScreen(window_, &position)) {
            throw std::runtime_error("Cannot locate scrolled test pixels");
        }
        return position;
    }
    POINT cursorPosition() const {
        POINT position{16, 16};
        if (!ClientToScreen(window_, &position)) {
            throw std::runtime_error("Cannot locate capture test cursor");
        }
        return position;
    }
    void scroll(COLORREF strip) {
        activeTexture_ = nullptr;
        color_ = strip;
        if (!ScrollWindowEx(window_, 16, 0, nullptr, nullptr, nullptr, nullptr, SW_INVALIDATE)) {
            throw std::runtime_error("Cannot scroll capture test surface");
        }
        UpdateWindow(window_);
        if (FAILED(DwmFlush())) {
            throw std::runtime_error("Desktop compositor unavailable");
        }
    }
    void paintMotion(unsigned frame) {
        if (textures_.empty()) {
            textures_.resize(3);
            for (unsigned texture = 0; texture < textures_.size(); ++texture) {
                auto& pixels = textures_[texture];
                pixels.resize(static_cast<std::size_t>(size_) * size_);
                for (int y = 0; y < size_; ++y) {
                    for (int x = 0; x < size_; ++x) {
                        std::uint32_t pattern = (x / 8 * 73856093u) ^ (y / 8 * 19349663u) ^ (texture * 83492791u);
                        pattern ^= pattern >> 13;
                        pattern *= 1274126177u;
                        pattern ^= pattern >> 16;
                        const auto shade = static_cast<std::uint8_t>(32 + pattern % 192);
                        pixels[static_cast<std::size_t>(y) * size_ + x] = RGB(shade, shade, shade);
                    }
                }
            }
        }
        activeTexture_ = &textures_[frame % textures_.size()];
        renderFailed_ = false;
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
        if (renderFailed_) {
            throw std::runtime_error("Cannot render motion test texture");
        }
        if (FAILED(DwmFlush())) {
            throw std::runtime_error("Desktop compositor unavailable");
        }
    }
    void paint(COLORREF color) {
        activeTexture_ = nullptr;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        color_ = color;
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
        if (FAILED(DwmFlush())) {
            throw std::runtime_error("Desktop compositor unavailable");
        }
    }

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<TestSurface*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            if (self->activeTexture_) {
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth = self->size_;
                bitmap.bmiHeader.biHeight = -self->size_;
                bitmap.bmiHeader.biPlanes = 1;
                bitmap.bmiHeader.biBitCount = 32;
                bitmap.bmiHeader.biCompression = BI_RGB;
                if (SetDIBitsToDevice(dc, 0, 0, self->size_, self->size_, 0, 0, 0, self->size_, self->activeTexture_->data(), &bitmap, DIB_RGB_COLORS) != self->size_) {
                    self->renderFailed_ = true;
                }
            } else {
                const auto brush = CreateSolidBrush(self->color_);
                FillRect(dc, &paint.rcPaint, brush);
                DeleteObject(brush);
            }
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    int size_;
    HWND window_ = nullptr;
    COLORREF color_ = RGB(0, 0, 0);
    std::vector<std::vector<std::uint32_t>> textures_;
    const std::vector<std::uint32_t>* activeTexture_ = nullptr;
    bool renderFailed_ = false;
};

} // namespace ultraterimal::test
