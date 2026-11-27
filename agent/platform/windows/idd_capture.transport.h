#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "capture.transport.h"

namespace ultraterimal::agent {

// Reads frames published by Ultraterimal's UMDF/IddCx virtual display driver
// via shared memory (zero WARP CopyResource+Map overhead when inSystemMemory=1).
class IddSharedCapture {
  public:
    IddSharedCapture();
    ~IddSharedCapture();
    IddSharedCapture(const IddSharedCapture&) = delete;
    IddSharedCapture& operator=(const IddSharedCapture&) = delete;

    // Returns true if the shared memory channel is open and the IDD driver is
    // actively publishing frames from system memory (inSystemMemory == 1).
    bool isActiveInSystemMemory() const noexcept;

    // Waits briefly for the IDD driver to start publishing after a topology switch.
    bool waitForDriverReady(std::chrono::milliseconds timeout) noexcept;

    // Requests the IDD driver to dynamically register a custom even resolution
    // (640x480..3840x2160) via IddCxMonitorUpdateModes so Windows accepts it.
    bool requestCustomMode(std::uint32_t width, std::uint32_t height,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) noexcept;

    // Reads the next frame (or the latest frame on first capture).
    // Returns false on timeout (no new frame); returns true when a frame was
    // delivered (either via directConsumer or into frame.bgra).
    bool acquireFrame(std::chrono::milliseconds timeout,
                      bool forceInitialFrame,
                      std::uint64_t captureEpoch,
                      std::uint64_t& sourceSerial,
                      CapturedFrame& frame,
                      std::vector<std::uint8_t>* reusablePixels,
                      CaptureTiming* timing,
                      const DesktopCapture::DirectPixelConsumer* directConsumer);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ultraterimal::agent
