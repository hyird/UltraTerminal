#include "deadline_timer.transport.h"

#include <algorithm>
#include <cstdint>
#include <system_error>

#include <windows.h>

namespace ultraterimal::agent {
WindowsDeadlineTimer::WindowsDeadlineTimer(asio::any_io_executor executor) : handle_(std::move(executor)) {
    auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (!timer && GetLastError() == ERROR_INVALID_PARAMETER) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    if (!timer) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create desktop deadline timer");
    }
    struct TimerOwner {
        HANDLE handle;
        ~TimerOwner() {
            if (handle) {
                CloseHandle(handle);
            }
        }
    } owner{timer};
    std::error_code error;
    handle_.assign(owner.handle, error);
    if (error) {
        throw std::system_error(error, "Attach desktop deadline timer");
    }
    owner.handle = nullptr;
}
void WindowsDeadlineTimer::expiresAt(Clock::time_point deadline) {
    cancel();
    deadline_ = deadline;
}
void WindowsDeadlineTimer::cancel() noexcept {
    CancelWaitableTimer(handle_.native_handle());
    std::error_code error;
    handle_.cancel(error);
}
void WindowsDeadlineTimer::arm() {
    using Tick = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<std::int64_t>(1, std::chrono::ceil<Tick>(deadline_ - Clock::now()).count());
    if (!SetWaitableTimer(handle_.native_handle(), &due, 0, nullptr, nullptr, FALSE)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Arm desktop deadline timer");
    }
}
} // namespace ultraterimal::agent
