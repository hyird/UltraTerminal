#include "idd_capture.transport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <windows.h>

#include <sddl.h>

#include "idd/idd_shared.protocol.h"

namespace ultraterimal::agent {
namespace {

using Clock = std::chrono::steady_clock;

std::uint32_t toMicroseconds(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(), 0, 60000000));
}

double toMilliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

struct SecurityDescriptorGuard {
    PSECURITY_DESCRIPTOR value = nullptr;
    ~SecurityDescriptorGuard() {
        if (value) {
            LocalFree(value);
        }
    }
};

} // namespace

struct IddSharedCapture::State {
    HANDLE mapping = nullptr;
    HANDLE event = nullptr;
    idd::SharedRegion* region = nullptr;
    std::uint64_t lastFrameSeq = 0;
    std::uint64_t lastDirectSeq = 0;

    ~State() {
        if (region) {
            UnmapViewOfFile(region);
        }
        if (mapping) {
            CloseHandle(mapping);
        }
        if (event) {
            CloseHandle(event);
        }
    }
};

IddSharedCapture::IddSharedCapture() : state_(std::make_unique<State>()) {
    // Allow LocalSystem (SY), Administrators (BA), and LOCAL SERVICE (LS, WUDFHost.exe).
    SecurityDescriptorGuard sd;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)", SDDL_REVISION_1, &sd.value, nullptr)) {
        sa.lpSecurityDescriptor = sd.value;
    }

    const ULARGE_INTEGER size{.QuadPart = sizeof(idd::SharedRegion)};
    state_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sa.lpSecurityDescriptor ? &sa : nullptr,
                                         PAGE_READWRITE, size.HighPart, size.LowPart, idd::kShmName);
    const DWORD mapErr = GetLastError();
    if (!state_->mapping) {
        return;
    }
    state_->region = static_cast<idd::SharedRegion*>(
        MapViewOfFile(state_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(idd::SharedRegion)));
    if (!state_->region) {
        CloseHandle(state_->mapping);
        state_->mapping = nullptr;
        return;
    }
    if (mapErr != ERROR_ALREADY_EXISTS) {
        state_->region->header.magic.store(idd::kShmMagic, std::memory_order_relaxed);
        state_->region->header.version.store(idd::kShmVersion, std::memory_order_release);
    }
    state_->event = CreateEventW(sa.lpSecurityDescriptor ? &sa : nullptr, FALSE, FALSE, idd::kEventName);
}

IddSharedCapture::~IddSharedCapture() = default;

bool IddSharedCapture::isActiveInSystemMemory() const noexcept {
    if (!state_ || !state_->region) {
        return false;
    }
    const auto& header = state_->region->header;
    if (header.magic.load(std::memory_order_acquire) != idd::kShmMagic ||
        header.version.load(std::memory_order_acquire) != idd::kShmVersion) {
        return false;
    }
    if (header.driverActive.load(std::memory_order_acquire) == 0 ||
        header.inSystemMemory.load(std::memory_order_acquire) == 0) {
        return false;
    }
    const auto heartbeat = header.heartbeatQpc.load(std::memory_order_acquire);
    if (heartbeat == 0) {
        return false;
    }
    LARGE_INTEGER now{}, freq{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
        return false;
    }
    const auto nowTicks = static_cast<std::uint64_t>(now.QuadPart);
    if (nowTicks > heartbeat && (nowTicks - heartbeat) > static_cast<std::uint64_t>(freq.QuadPart) * 5u) {
        return false;
    }
    return true;
}

bool IddSharedCapture::waitForDriverReady(std::chrono::milliseconds timeout) noexcept {
    if (!state_ || !state_->region) {
        return false;
    }
    auto hasFrame = [&]() noexcept {
        return isActiveInSystemMemory() && state_->region->header.frameSeq.load(std::memory_order_acquire) > 0;
    };
    if (hasFrame()) {
        return true;
    }
    bool nudged = false;
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (hasFrame()) {
            return true;
        }
        if (!nudged && isActiveInSystemMemory()) {
            nudged = true;
            // Nudge DWM to compose a frame if the desktop is completely static.
            (void)InvalidateRect(nullptr, nullptr, FALSE);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        if (state_->event) {
            WaitForSingleObject(state_->event, static_cast<DWORD>(std::min<std::int64_t>(remaining.count(), 50)));
        } else {
            Sleep(10);
        }
    }
    return hasFrame();
}

bool IddSharedCapture::requestCustomMode(std::uint32_t width, std::uint32_t height,
                                         std::chrono::milliseconds timeout) noexcept {
    if (!state_ || !state_->region || !idd::isValidCustomMode(width, height) || !isActiveInSystemMemory()) {
        return false;
    }
    auto& header = state_->region->header;
    header.requestedWidth.store(width, std::memory_order_relaxed);
    header.requestedHeight.store(height, std::memory_order_relaxed);
    const auto seq = header.requestedModeSeq.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (header.appliedModeSeq.load(std::memory_order_acquire) == seq) {
            return true;
        }
        Sleep(10);
    }
    return header.appliedModeSeq.load(std::memory_order_acquire) == seq;
}

bool IddSharedCapture::acquireFrame(std::chrono::milliseconds timeout,
                                    bool forceInitialFrame,
                                    std::uint64_t captureEpoch,
                                    std::uint64_t& sourceSerial,
                                    CapturedFrame& frame,
                                    std::vector<std::uint8_t>* reusablePixels,
                                    CaptureTiming* timing,
                                    const DesktopCapture::DirectPixelConsumer* directConsumer) {
    if (!state_ || !state_->region) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_DEVICE_LOST);
    }
    const auto acquireStarted = timing ? Clock::now() : Clock::time_point{};
    auto& header = state_->region->header;

    auto currentSeq = header.frameSeq.load(std::memory_order_acquire);
    if (currentSeq == state_->lastFrameSeq && !(forceInitialFrame && currentSeq > 0)) {
        if (timeout.count() > 0 && state_->event) {
            (void)WaitForSingleObject(state_->event, static_cast<DWORD>(timeout.count()));
            currentSeq = header.frameSeq.load(std::memory_order_acquire);
        }
    }
    const auto acquireEnded = timing ? Clock::now() : Clock::time_point{};
    if (timing) {
        timing->desktopImageInSystemMemory = true;
        timing->acquireMilliseconds = toMilliseconds(acquireStarted, acquireEnded);
        timing->stages.acquireWaitMicroseconds = toMicroseconds(acquireStarted, acquireEnded);
    }

    if (!isActiveInSystemMemory()) {
        throw CaptureReset(wire::DesktopFault::CAPTURE_DEVICE_LOST);
    }
    if (currentSeq == 0 || (currentSeq == state_->lastFrameSeq && !forceInitialFrame)) {
        return false;
    }

    const std::uint32_t slotIndex = header.activeSlot.load(std::memory_order_acquire) & 1u;
    const idd::SlotMetadata meta = header.slots[slotIndex];
    if (meta.frameSeq == 0 || meta.width == 0 || meta.height == 0 ||
        meta.width > idd::kMaxWidth || meta.height > idd::kMaxHeight ||
        meta.format != idd::kFormatBgra8 || meta.pitch < meta.width * 4u) {
        return false;
    }
    const std::size_t frameBytes = static_cast<std::size_t>(meta.pitch) * meta.height;
    if (frameBytes > idd::kMaxFrameBytes) {
        return false;
    }

    if (sourceSerial == std::numeric_limits<std::uint64_t>::max()) {
        throw CaptureReset();
    }
    ++sourceSerial;

    const bool sequential = (meta.frameSeq == state_->lastFrameSeq + 1u) && (meta.fullFrame == 0u);
    const std::uint32_t rowBegin = sequential ? std::min(meta.dirtyRowBegin, meta.height) : 0u;
    const std::uint32_t rowEnd = sequential ? std::min(meta.dirtyRowEnd, meta.height) : meta.height;
    const bool incremental = sequential && rowBegin < rowEnd;

    frame.width = meta.width;
    frame.height = meta.height;
    frame.rotation = 1; // DXGI_MODE_ROTATION_IDENTITY
    frame.captureTicks = meta.qpcTicks;
    frame.captureEpoch = captureEpoch;
    frame.sourceSerial = sourceSerial;
    frame.dirtyRowBegin = incremental ? rowBegin : 0u;
    frame.dirtyRowEnd = incremental ? rowEnd : meta.height;
    frame.changedPixels = incremental ? meta.changedPixels : static_cast<std::uint64_t>(meta.width) * meta.height;

    const std::uint8_t* slotPixels = state_->region->pixels[slotIndex];
    const auto copyStarted = timing ? Clock::now() : Clock::time_point{};
    bool consumedDirectly = false;
    if (directConsumer && *directConsumer) {
        const bool allowDirectInc = incremental && (state_->lastDirectSeq == state_->lastFrameSeq);
        consumedDirectly = (*directConsumer)(
            std::span<const std::uint8_t>(slotPixels, frameBytes),
            static_cast<std::size_t>(meta.pitch),
            meta.width, meta.height,
            allowDirectInc ? rowBegin : 0u,
            allowDirectInc ? rowEnd : meta.height,
            captureEpoch, sourceSerial, allowDirectInc);
        if (consumedDirectly) {
            state_->lastDirectSeq = meta.frameSeq;
            frame.bgra.clear();
        }
    }
    if (!consumedDirectly) {
        state_->lastDirectSeq = 0;
        const std::size_t compactBytes = static_cast<std::size_t>(meta.width) * meta.height * 4u;
        if (reusablePixels) {
            frame.bgra.swap(*reusablePixels);
        }
        frame.bgra.resize(compactBytes);
        const std::size_t rowBytes = static_cast<std::size_t>(meta.width) * 4u;
        if (meta.pitch == rowBytes) {
            std::memcpy(frame.bgra.data(), slotPixels, compactBytes);
        } else {
            for (std::uint32_t y = 0; y < meta.height; ++y) {
                std::memcpy(frame.bgra.data() + static_cast<std::size_t>(y) * rowBytes,
                            slotPixels + static_cast<std::size_t>(y) * meta.pitch, rowBytes);
            }
        }
    }
    const auto copyEnded = timing ? Clock::now() : Clock::time_point{};
    if (timing) {
        timing->mapMilliseconds = 0;
        timing->rowCopyMilliseconds = toMilliseconds(copyStarted, copyEnded);
        timing->stages.mapMicroseconds = 0;
        timing->stages.rowCopyMicroseconds = toMicroseconds(copyStarted, copyEnded);
    }

    state_->lastFrameSeq = meta.frameSeq;
    return true;
}

} // namespace ultraterimal::agent
