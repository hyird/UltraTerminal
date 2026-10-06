#pragma once

#include <cstdint>
#include <optional>

namespace ultraterimal::agent {
// A process-local request, not a guarantee of timer precision (Windows may
// ignore it for occluded processes). One paired request per active episode.
struct WindowsTimerResolutionStatus {
    bool requested = false, active = false;
    std::uint32_t periodMilliseconds = 1;
    std::optional<std::uint32_t> capabilitiesResult, beginResult, endResult;
};
class WindowsTimerResolution {
  public:
    WindowsTimerResolution() = default;
    ~WindowsTimerResolution();
    WindowsTimerResolution(const WindowsTimerResolution&) = delete;
    WindowsTimerResolution& operator=(const WindowsTimerResolution&) = delete;
    void setRequested(bool requested) noexcept;
    WindowsTimerResolutionStatus status() const noexcept { return status_; }

  private:
    WindowsTimerResolutionStatus status_;
};
} // namespace ultraterimal::agent
