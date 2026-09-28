#pragma once
#include <chrono>
#include <expected>
#include <mfidl.h>

#include <windows.h>

#include <wrl/client.h>

namespace ultraterimal::agent {
// One worker owns next()/stop(). The MF callback only completes one event slot.
// Stop shuts down the source and releases a pending callback. Never use on an I/O loop.
class MediaEvents {
  public:
    explicit MediaEvents(IMFMediaEventGenerator* source);
    ~MediaEvents();
    MediaEvents(const MediaEvents&) = delete;
    MediaEvents& operator=(const MediaEvents&) = delete;
    std::expected<Microsoft::WRL::ComPtr<IMFMediaEvent>, HRESULT> next(std::chrono::milliseconds timeout);
    void stop() noexcept;

  private:
    struct State;
    State* state_;
};
} // namespace ultraterimal::agent
