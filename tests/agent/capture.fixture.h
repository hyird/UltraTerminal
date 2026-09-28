#pragma once
#include <stdexcept>

#include <windows.h>

#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
namespace ultraterimal::test {
class TestSurface {
  public:
    TestSurface() {
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
                                  bounds.left + 32, bounds.top + 32, 64, 64,
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
    POINT cursorPosition() const {
        POINT position{16, 16};
        if (!ClientToScreen(window_, &position)) {
            throw std::runtime_error("Cannot locate capture test cursor");
        }
        return position;
    }
    void paint(COLORREF color) {
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
        const auto* self = reinterpret_cast<TestSurface*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            const auto dc = BeginPaint(window, &paint);
            const auto brush = CreateSolidBrush(self->color_);
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    HWND window_ = nullptr;
    COLORREF color_ = RGB(0, 0, 0);
};

} // namespace ultraterimal::test
