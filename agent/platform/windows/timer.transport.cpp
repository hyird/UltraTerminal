#include "timer.transport.h"

// timeapi.h requires Windows SDK types; preserve this dependency order.
// clang-format off
#include <windows.h>
#include <timeapi.h>
// clang-format on

namespace ultraterimal::agent {
WindowsTimerResolution::~WindowsTimerResolution() { setRequested(false); }
void WindowsTimerResolution::setRequested(bool requested) noexcept {
    if (requested == status_.requested) {
        return;
    }
    if (!requested) {
        if (status_.active) {
            status_.endResult = timeEndPeriod(status_.periodMilliseconds);
            status_.active = false;
        }
        status_.requested = false;
        return;
    }
    status_.requested = true;
    status_.beginResult.reset();
    status_.endResult.reset();
    TIMECAPS caps{};
    status_.capabilitiesResult = timeGetDevCaps(&caps, sizeof(caps));
    if (*status_.capabilitiesResult != TIMERR_NOERROR) {
        return;
    }
    if (caps.wPeriodMin > status_.periodMilliseconds || caps.wPeriodMax < status_.periodMilliseconds) {
        status_.beginResult = TIMERR_NOCANDO;
        return;
    }
    status_.beginResult = timeBeginPeriod(status_.periodMilliseconds);
    status_.active = *status_.beginResult == TIMERR_NOERROR;
}
} // namespace ultraterimal::agent
