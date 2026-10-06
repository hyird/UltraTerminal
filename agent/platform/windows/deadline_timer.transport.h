#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/windows/object_handle.hpp>
#include <chrono>
#include <utility>

namespace ultraterimal::agent {
// Kernel deadline notifications return to the owning Asio executor.
class WindowsDeadlineTimer {
  public:
    using Clock = std::chrono::steady_clock;
    explicit WindowsDeadlineTimer(asio::any_io_executor executor);
    WindowsDeadlineTimer(const WindowsDeadlineTimer&) = delete;
    WindowsDeadlineTimer& operator=(const WindowsDeadlineTimer&) = delete;
    void expiresAt(Clock::time_point deadline);
    void expiresAfter(Clock::duration duration) { expiresAt(Clock::now() + duration); }
    void cancel() noexcept;
    template <class CompletionToken>
    auto asyncWait(CompletionToken&& token) {
        arm();
        return handle_.async_wait(std::forward<CompletionToken>(token));
    }

  private:
    void arm();
    asio::windows::object_handle handle_;
    Clock::time_point deadline_{};
};
} // namespace ultraterimal::agent
