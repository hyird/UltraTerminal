#include <array>
#include <charconv>
#include <cstddef>
#include <iostream>
#include <string_view>

#include <windows.h>

#include <dxgi1_2.h>
#include <wrl/client.h>

#include "platform/windows/input.transport.h"

namespace {
struct Event {
    DWORD message = 0, code = 0, flags = 0;
    LONG x = 0, y = 0;
    bool mouse = false;
};
std::array<Event, 256> events;
std::size_t count = 0;
bool overflow = false;
HWND patternWindow = nullptr;
bool alternatePattern = false;
bool animatePattern = false;
unsigned animationStep = 0;
LRESULT CALLBACK pattern(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        constexpr std::array<COLORREF, 4> colors{RGB(208, 32, 32), RGB(32, 208, 32), RGB(32, 32, 208), RGB(208, 208, 32)};
        for (int index = 0; index < 4; ++index) {
            const RECT area{index * 64, 0, (index + 1) * 64, 128};
            const auto color = animatePattern && index == 0 ? RGB(animationStep, 255 - animationStep, 96) : colors[alternatePattern ? 3 - index : index];
            const auto brush = CreateSolidBrush(color);
            FillRect(dc, &area, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const auto& value = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if ((value.flags & LLKHF_INJECTED) && value.dwExtraInfo == ultraterimal::agent::kDesktopInputMarker) {
            if (value.vkCode == VK_F23 && message == WM_KEYDOWN && patternWindow) {
                alternatePattern = !alternatePattern;
                InvalidateRect(patternWindow, nullptr, FALSE);
            }
            if (value.vkCode == VK_F22 && message == WM_KEYDOWN && patternWindow) {
                animatePattern = !animatePattern;
                alternatePattern = false;
                InvalidateRect(patternWindow, nullptr, FALSE);
            }
            if (count < events.size()) {
                events[count++] = {static_cast<DWORD>(message), value.vkCode, value.flags};
            } else {
                overflow = true;
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const auto& value = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if ((value.flags & LLMHF_INJECTED) && value.dwExtraInfo == ultraterimal::agent::kDesktopInputMarker) {
            if (count < events.size()) {
                events[count++] = {static_cast<DWORD>(message), value.mouseData, value.flags, value.pt.x, value.pt.y, true};
            } else {
                overflow = true;
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--probe") {
        INPUT probe[2]{};
        for (auto& input : probe) {
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_F24;
            input.ki.dwExtraInfo = ultraterimal::agent::kDesktopInputMarker;
        }
        probe[1].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(2, probe, sizeof(INPUT)) == 2 ? 0 : 1;
    }
    unsigned durationSeconds = 150;
    bool startAnimated = false;
    const int durationArgument = argc == 4 && std::string_view(argv[1]) == "--animate" && std::string_view(argv[2]) == "--duration" ? 3 : argc == 3 && std::string_view(argv[1]) == "--duration" ? 2
                                                                                                                                                                                                 : 0;
    if (durationArgument) {
        startAnimated = durationArgument == 3;
        const std::string_view value(argv[durationArgument]);
        const auto result = std::from_chars(value.data(), value.data() + value.size(), durationSeconds);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || durationSeconds < 150 || durationSeconds > 3750) {
            return 2;
        }
    } else if (argc != 1) {
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Capture display indices follow DXGI adapter/output order, which need not
    // place the primary monitor first. Match the Agent's default display.
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return 1;
    }
    RECT bounds{};
    bool found = false;
    for (UINT adapterIndex = 0; !found; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, &adapter) != S_OK) {
            break;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, &output) != S_OK) {
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
        return 1;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = pattern;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"UltraterimalCaptureVerification";
    if (!RegisterClassW(&windowClass)) {
        return 1;
    }
    const auto window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
                                        L"Ultraterimal capture verification", WS_POPUP, bounds.left + 32, bounds.top + 32, 256, 128,
                                        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        return 1;
    }
    // The launcher hides the console through STARTF_USESHOWWINDOW. Do not let
    // its SW_HIDE override the first ShowWindow call for the capture fixture.
    if (!SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) ||
        !IsWindowVisible(window)) {
        DestroyWindow(window);
        return 1;
    }
    patternWindow = window;
    animatePattern = startAnimated;
    UpdateWindow(window);
    // Keep the interactive test desktop available while observing injected input.
    // Windows clears this thread-scoped requirement when the fixture exits.
    if (!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) {
        return 1;
    }
    const auto keys = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, GetModuleHandleW(nullptr), 0);
    const auto pointer = SetWindowsHookExW(WH_MOUSE_LL, mouse, GetModuleHandleW(nullptr), 0);
    if (!keys || !pointer) {
        if (keys) {
            UnhookWindowsHookEx(keys);
        }
        if (pointer) {
            UnhookWindowsHookEx(pointer);
        }
        return 1;
    }
    const auto timer = SetTimer(nullptr, 0, 10, nullptr);
    if (!timer) {
        UnhookWindowsHookEx(keys);
        UnhookWindowsHookEx(pointer);
        return 1;
    }
    std::cout << "{\"ready\":true}\n"
              << std::flush;
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(durationSeconds) * 1000;
    MSG message{};
    while (GetTickCount64() < deadline && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_TIMER && message.wParam == timer && animatePattern) {
            animationStep = (animationStep + 17) % 256;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (overflow) {
            break;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& event = events[i];
            std::cout << "{\"mouse\":" << (event.mouse ? "true" : "false") << ",\"message\":" << event.message
                      << ",\"code\":" << event.code << ",\"flags\":" << event.flags << ",\"x\":" << event.x << ",\"y\":" << event.y << "}\n";
        }
        if (count) {
            std::cout << std::flush;
        }
        count = 0;
    }
    KillTimer(nullptr, timer);
    UnhookWindowsHookEx(keys);
    UnhookWindowsHookEx(pointer);
    DestroyWindow(window);
    return overflow ? 1 : 0;
}
