#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <windows.h>

#include "platform/windows/input.transport.h"

namespace {
using namespace ultraterimal;
std::vector<bool> keys;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
LRESULT CALLBACK keyboard(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
        if (event.vkCode == VK_F24 && (event.flags & LLKHF_INJECTED)) {
            if (keys.size() < keys.capacity()) {
                keys.push_back(message == WM_KEYDOWN || message == WM_SYSKEYDOWN);
            }
            return 1; // Consume only this fixture's injected key, never user input.
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
class Hook {
  public:
    Hook() : handle_(SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, GetModuleHandleW(nullptr), 0)) {
        require(handle_ != nullptr, "Cannot observe test input");
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    }
    ~Hook() { UnhookWindowsHookEx(handle_); }

  private:
    HHOOK handle_;
};
void pump(std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (keys.size() < count && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (keys.size() < count) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        }
    }
    if (keys.size() != count) {
        std::cerr << "Injected F24 transitions expected=" << count << " observed=" << keys.size() << '\n';
    }
    require(keys.size() == count, "Missing or extra injected key transitions");
}
template <class F>
void rejected(F&& action) {
    bool failed = false;
    try {
        action();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "Invalid input or competing controller accepted");
}
} // namespace
int main() {
    try {
        keys.reserve(64); // No allocation or exception may cross the hook callback.
        Hook hook;
        const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        require(previous != nullptr, "Cannot select physical coordinates");
        const auto width = static_cast<std::uint32_t>(GetSystemMetrics(SM_CXSCREEN));
        const auto height = static_cast<std::uint32_t>(GetSystemMetrics(SM_CYSCREEN));
        SetThreadDpiAwarenessContext(previous);
        wire::InputEvent key;
        key.set_kind(wire::InputEvent::KEY);
        key.set_code(VK_F24);
        key.set_pressed(true);
        {
            agent::DesktopInput input(0, 0, width, height);
            rejected([&] { agent::DesktopInput competing(0, 0, width, height); });
            wire::InputEvent invalid;
            invalid.set_kind(wire::InputEvent::POINTER_MOVE);
            invalid.set_x(static_cast<std::int32_t>(width));
            rejected([&] { input.apply(invalid); });
            invalid.set_kind(wire::InputEvent::KEY);
            invalid.set_code(0xffffffff);
            rejected([&] { input.apply(invalid); });
            input.apply(key);
            pump(1);
            input.apply(key); // Repeated keydown retains one owned release.
            pump(2);
            input.releaseAll();
            pump(3);
            input.releaseAll();
            key.set_pressed(false);
            input.apply(key); // An unowned keyup must not affect other input.
            key.set_pressed(true);
            input.apply(key);
            pump(4);
        }
        pump(5); // Destructor releases the held key before releasing control.
        require(keys == std::vector<bool>({true, true, false, true, false}), "Input order/release mismatch");
        agent::DesktopInput next(0, 0, width, height);
        for (unsigned attempt = 0; attempt < 5; ++attempt) {
            keys.clear();
            for (unsigned pair = 0; pair < 12; ++pair) {
                key.set_pressed(true);
                next.apply(key);
                key.set_pressed(false);
                next.apply(key);
            }
            pump(24);
            for (std::size_t index = 0; index < keys.size(); ++index) {
                require(keys[index] == (index % 2 == 0), "Rapid Windows key transitions were reordered");
            }
        }
        {
            keys.clear();
            rejected([&] { agent::DesktopInput competing(0, 0, width, height); });
            std::promise<void> preemptReady;
            std::promise<void> exclusiveTried;
            std::exception_ptr workerFailure;
            std::thread worker([&] {
                try {
                    agent::DesktopInput preempt(0, 0, width, height, agent::DesktopInput::Ownership::kPreempt);
                    preemptReady.set_value();
                    exclusiveTried.get_future().wait();
                    wire::InputEvent k;
                    k.set_kind(wire::InputEvent::KEY);
                    k.set_code(VK_F24);
                    k.set_pressed(true);
                    preempt.apply(k);
                    k.set_pressed(false);
                    preempt.apply(k);
                } catch (...) {
                    workerFailure = std::current_exception();
                    try {
                        preemptReady.set_value();
                    } catch (...) {
                    }
                }
            });
            preemptReady.get_future().wait();
            key.set_pressed(true);
            next.apply(key);
            key.set_pressed(false);
            next.apply(key);
            exclusiveTried.set_value();
            pump(2);
            worker.join();
            if (workerFailure) {
                std::rethrow_exception(workerFailure);
            }
            require(keys == std::vector<bool>({true, false}), "Exclusive input failed to yield during preemption");
            key.set_pressed(true);
            next.apply(key);
            key.set_pressed(false);
            next.apply(key);
            pump(4);
            require(keys == std::vector<bool>({true, false, true, false}), "Exclusive input did not resume after preemption");
        }
        std::cout << "Real Windows key injection, owned release, bounds, control lease, preemption and 120 rapid transitions passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
