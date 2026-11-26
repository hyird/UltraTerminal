#include "input.transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <vector>

#include <windows.h>

#include "desktop.transport.h"
#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
class ControlLease {
  public:
    ControlLease() {
        handle_.reset(CreateMutexW(nullptr, TRUE, L"Local\\UltraterimalDesktopInput"));
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("Desktop write control is unavailable");
        }
    }
    ~ControlLease() { ReleaseMutex(handle_.get()); }

  private:
    UniqueHandle handle_;
};
class DpiScope {
  public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        if (!previous_) {
            throw std::runtime_error("Cannot select physical desktop coordinates");
        }
    }
    ~DpiScope() { SetThreadDpiAwarenessContext(previous_); }

  private:
    DPI_AWARENESS_CONTEXT previous_;
};
struct VirtualDesktop {
    int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bool operator==(const VirtualDesktop&) const = default;
};
INPUT keyInput(std::uint32_t code, bool pressed) {
    const auto key = static_cast<WORD>(code & 0xff);
    const bool extended = code == 0x10d || key == VK_RCONTROL || key == VK_RMENU || key == VK_DIVIDE || key == VK_NUMLOCK ||
                          key == VK_CANCEL || key == VK_SNAPSHOT || key == VK_LWIN || key == VK_RWIN || key == VK_APPS ||
                          (key >= VK_PRIOR && key <= VK_DOWN) || key == VK_INSERT || key == VK_DELETE;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwExtraInfo = kDesktopInputMarker;
    input.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0U) | (pressed ? 0U : KEYEVENTF_KEYUP);
    return input;
}
constexpr std::array<int, 5> kButtonKeys{VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2};
INPUT buttonInput(std::uint32_t code, bool pressed) {
    constexpr std::array<DWORD, 5> down{MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_XDOWN, MOUSEEVENTF_XDOWN};
    constexpr std::array<DWORD, 5> up{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_XUP, MOUSEEVENTF_XUP};
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = kDesktopInputMarker;
    input.mi.dwFlags = pressed ? down[code] : up[code];
    if (code >= 3) {
        input.mi.mouseData = code == 3 ? XBUTTON1 : XBUTTON2;
    }
    return input;
}
void submit(INPUT input) {
    if (input.type == INPUT_MOUSE) {
        input.mi.dwExtraInfo = kDesktopInputMarker;
    }
    if (SendInput(1, &input, sizeof(input)) != 1) {
        throw std::runtime_error("Windows rejected desktop input");
    }
}
LONG normalized(std::int64_t position, std::int64_t origin, int extent) {
    // Target the pixel center to avoid rounding into a neighboring physical pixel.
    return static_cast<LONG>(std::clamp(((position - origin) * 65536 + 32768) / extent, std::int64_t{0}, std::int64_t{65535}));
}
} // namespace
struct DesktopInput::State {
    ControlLease lease;
    std::array<bool, 512> keys{};
    std::array<bool, 5> buttons{};
    std::int32_t left, top;
    std::uint32_t width, height;
    VirtualDesktop desktop;
    DWORD thread = GetCurrentThreadId();
    std::uint64_t desktopGeneration = 0;
    std::chrono::steady_clock::time_point lastDesktopCheck{};
    bool stale = false;

    State(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h) : left(x), top(y), width(w), height(h) {
        desktopGeneration = requireInputDesktop();
        DpiScope dpi;
        desktop = VirtualDesktop{};
        lastDesktopCheck = std::chrono::steady_clock::now();
        if (!width || !height || width > 16384 || height > 16384 || desktop.width <= 0 || desktop.height <= 0 ||
            left < desktop.left || top < desktop.top || static_cast<std::int64_t>(left) + width > static_cast<std::int64_t>(desktop.left) + desktop.width ||
            static_cast<std::int64_t>(top) + height > static_cast<std::int64_t>(desktop.top) + desktop.height) {
            throw std::invalid_argument("Input display is outside the virtual desktop");
        }
    }
    void requireThread() const {
        if (GetCurrentThreadId() != thread) {
            throw std::logic_error("Desktop input belongs to its creating thread");
        }
    }
    void release() {
        requireThread();
        std::vector<INPUT> inputs;
        std::vector<bool*> owned;
        inputs.reserve(keys.size() + buttons.size());
        owned.reserve(keys.size() + buttons.size());
        for (std::uint32_t code = 0; code < keys.size(); ++code) {
            if (keys[code]) {
                inputs.push_back(keyInput(code, false));
                owned.push_back(&keys[code]);
            }
        }
        for (std::uint32_t code = 0; code < buttons.size(); ++code) {
            if (buttons[code]) {
                inputs.push_back(buttonInput(code, false));
                owned.push_back(&buttons[code]);
            }
        }
        if (inputs.empty()) {
            return;
        }
        const auto count = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        for (std::size_t i = 0; i < count; ++i) {
            *owned[i] = false;
        }
        if (count != inputs.size()) {
            throw std::runtime_error("Windows could not release all owned input");
        }
    }
    void apply(const wire::InputEvent& event) {
        requireThread();
        if (!protocol::validateInputEvent(event)) {
            throw std::invalid_argument("Invalid desktop input event");
        }
        if (event.kind() == wire::InputEvent::RELEASE_ALL) {
            release();
            return;
        }
        // Wait for the video owner to publish a new configuration and recreate
        // this input binding. Never apply coordinates from the previous desktop.
        const auto now = std::chrono::steady_clock::now();
        const bool verifyDesktop = event.kind() != wire::InputEvent::POINTER_MOVE ||
                                   now - lastDesktopCheck >= std::chrono::milliseconds(100);
        if (verifyDesktop) {
            try {
                if (requireInputDesktop() != desktopGeneration) {
                    stale = true;
                }
            } catch (const DesktopChanged&) {
                stale = true;
            }
            if (stale) {
                release();
                return;
            }
            DpiScope dpi;
            if (!(desktop == VirtualDesktop{})) {
                throw std::runtime_error("Desktop geometry changed during input");
            }
            lastDesktopCheck = now;
        } else if (stale) {
            release();
            return;
        }
        INPUT input{};
        input.type = INPUT_MOUSE;
        switch (event.kind()) {
        case wire::InputEvent::POINTER_MOVE:
            if (static_cast<std::uint32_t>(event.x()) >= width || static_cast<std::uint32_t>(event.y()) >= height) {
                throw std::invalid_argument("Pointer is outside the selected display");
            }
            input.mi.dx = normalized(static_cast<std::int64_t>(left) + event.x(), desktop.left, desktop.width);
            input.mi.dy = normalized(static_cast<std::int64_t>(top) + event.y(), desktop.top, desktop.height);
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            submit(input);
            break;
        case wire::InputEvent::KEY: {
            auto& held = keys[event.code()];
            if (!event.pressed() && !held) {
                return;
            }
            if (event.pressed() && !held && (GetAsyncKeyState(static_cast<int>(event.code() & 0xff)) & 0x8000)) {
                throw std::runtime_error("Key is already held outside this input session");
            }
            submit(keyInput(event.code(), event.pressed()));
            held = event.pressed();
            break;
        }
        case wire::InputEvent::BUTTON: {
            auto& held = buttons[event.code()];
            if (!event.pressed() && !held) {
                return;
            }
            if (event.pressed() && !held && (GetAsyncKeyState(kButtonKeys[event.code()]) & 0x8000)) {
                throw std::runtime_error("Button is already held outside this input session");
            }
            if (held == event.pressed()) {
                return;
            }
            submit(buttonInput(event.code(), event.pressed()));
            held = event.pressed();
            break;
        }
        case wire::InputEvent::WHEEL:
            input.mi.dwFlags = event.code() ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(event.wheel_delta());
            submit(input);
            break;
        default:
            throw std::invalid_argument("Unsupported desktop input event");
        }
    }
};
DesktopInput::DesktopInput(std::int32_t left, std::int32_t top, std::uint32_t width, std::uint32_t height)
    : state_(std::make_unique<State>(left, top, width, height)) {}
DesktopInput::~DesktopInput() {
    try {
        state_->release();
    } catch (...) { /* Best effort during desktop/session teardown. */
    }
}
void DesktopInput::apply(const wire::InputEvent& event) { state_->apply(event); }
void DesktopInput::releaseAll() { state_->release(); }
bool DesktopInput::needsRebind() const {
    state_->requireThread();
    return state_->stale;
}
} // namespace ultraterimal::agent
