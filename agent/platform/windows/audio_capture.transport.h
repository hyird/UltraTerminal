#pragma once

#include <memory>
#include <vector>

namespace ultraterimal::agent {

// Captures system render audio via WASAPI shared loopback.
// All methods must run on one COM-initialized (COINIT_MULTITHREADED) thread.
class WindowsAudioCapture {
  public:
    WindowsAudioCapture();
    ~WindowsAudioCapture();
    WindowsAudioCapture(const WindowsAudioCapture&) = delete;
    WindowsAudioCapture& operator=(const WindowsAudioCapture&) = delete;

    bool openDefaultEndpoint();
    bool readAvailable(std::vector<float>& outStereo48k);
    void closeEndpoint() noexcept;
    bool isOpen() const noexcept;
    void* deviceChangeHandle() const noexcept;
    void resetDeviceChangeEvent() noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ultraterimal::agent
