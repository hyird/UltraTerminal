#include "audio.runtime.h"

#include <algorithm>
#include <asio_bridge.h>
#include <atomic>
#include <channel.h>
#include <chrono>
#include <deque>
#include <mutex>
#include <objbase.h>
#include <optional>
#include <task.h>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include "audio.protocol.h"
#include "audio_codec.transport.h"
#include "platform/windows/audio_capture.transport.h"

namespace ultraterimal::agent {
namespace {

ruvia::task<ruvia::wait_result<bool>> awaitWake(ruvia::channel_receiver<bool>& receiver) {
    co_return co_await receiver.receive();
}

constexpr std::size_t kMaxBufferedAudioFrames = 6;
constexpr std::size_t kMaxBufferedPcmFloats = kAudioFrameSamples * kAudioChannels * 10;
constexpr unsigned kTrailingSilentFrames = 5;

class ComApartmentGuard {
  public:
    ComApartmentGuard() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = (hr == S_OK || hr == S_FALSE);
    }
    ~ComApartmentGuard() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    ComApartmentGuard(const ComApartmentGuard&) = delete;
    ComApartmentGuard& operator=(const ComApartmentGuard&) = delete;

  private:
    bool initialized_ = false;
};

wire::AudioConfig makeAudioConfig(bool available) {
    wire::AudioConfig config;
    config.set_available(available);
    if (available) {
        config.set_codec("opus");
        config.set_sample_rate(kAudioSampleRate);
        config.set_channels(kAudioChannels);
        config.set_frame_duration_microseconds(kAudioFrameDurationMicroseconds);
    }
    return config;
}

} // namespace

struct DesktopAudioRuntime::State {
    ruvia::event_loop loop;
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> wake;
    HANDLE stopEvent = nullptr;
    std::mutex mutex;
    std::optional<wire::AudioConfig> pendingConfig;
    std::deque<wire::AudioFrame> frames;
    std::atomic_bool stopThread{false};
    bool enabled = false;
    bool stopped = false;
    std::thread thread;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::uint64_t lastTimestampUs = 0;

    explicit State(ruvia::event_loop owner)
        : loop(std::move(owner)), wake(ruvia::make_channel<bool>(loop.worker(), {.capacity_ = 1})),
          stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ~State() {
        stopAll();
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }

    void signalWake() noexcept {
        (void)wake.first.send(true);
    }

    void publishConfig(bool available) {
        {
            std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            pendingConfig = makeAudioConfig(available);
        }
        signalWake();
    }

    void publishFrame(wire::AudioFrame frame) {
        {
            std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
            while (frames.size() >= kMaxBufferedAudioFrames) {
                frames.pop_front();
            }
            frames.push_back(std::move(frame));
        }
        signalWake();
    }

    std::uint64_t nextTimestamp() noexcept {
        const auto nowUs = static_cast<std::uint64_t>(
            std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch).count()));
        const std::uint64_t ts = std::max(nowUs, lastTimestampUs + 1);
        lastTimestampUs = ts;
        return ts;
    }

    void waitInterruptible(void* deviceChangeHandle, DWORD timeoutMs) noexcept {
        if (!stopEvent) {
            Sleep(timeoutMs);
            return;
        }
        if (deviceChangeHandle) {
            HANDLE handles[2]{stopEvent, static_cast<HANDLE>(deviceChangeHandle)};
            (void)WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
        } else {
            (void)WaitForSingleObject(stopEvent, timeoutMs);
        }
    }

    void runCapture() noexcept {
        try {
            ComApartmentGuard com;
            WindowsAudioCapture capture;
            AudioOpusEncoder encoder;
            std::vector<float> pcm;
            pcm.reserve(kMaxBufferedPcmFloats);
            std::optional<bool> lastAvailable;
            DWORD backoffMs = 500;
            bool inSilence = true;
            unsigned trailingSilent = 0;

            while (!stopThread.load(std::memory_order_acquire)) {
                if (!capture.isOpen()) {
                    capture.resetDeviceChangeEvent();
                    if (capture.openDefaultEndpoint()) {
                        backoffMs = 500;
                        if (lastAvailable != true) {
                            lastAvailable = true;
                            publishConfig(true);
                        }
                    } else {
                        if (lastAvailable != false) {
                            lastAvailable = false;
                            publishConfig(false);
                        }
                        waitInterruptible(capture.deviceChangeHandle(), backoffMs);
                        if (capture.deviceChangeHandle() &&
                            WaitForSingleObject(static_cast<HANDLE>(capture.deviceChangeHandle()), 0) == WAIT_OBJECT_0) {
                            capture.resetDeviceChangeEvent();
                            backoffMs = 500;
                        } else {
                            backoffMs = std::min<DWORD>(backoffMs * 2, 2000);
                        }
                        continue;
                    }
                }

                if (capture.deviceChangeHandle() &&
                    WaitForSingleObject(static_cast<HANDLE>(capture.deviceChangeHandle()), 0) == WAIT_OBJECT_0) {
                    capture.closeEndpoint();
                    capture.resetDeviceChangeEvent();
                    pcm.clear();
                    encoder.reset();
                    inSilence = true;
                    trailingSilent = 0;
                    continue;
                }

                if (!capture.readAvailable(pcm)) {
                    capture.closeEndpoint();
                    pcm.clear();
                    encoder.reset();
                    inSilence = true;
                    trailingSilent = 0;
                    if (lastAvailable != false) {
                        lastAvailable = false;
                        publishConfig(false);
                    }
                    waitInterruptible(capture.deviceChangeHandle(), 200);
                    continue;
                }

                if (pcm.size() > kMaxBufferedPcmFloats) {
                    const std::size_t excess = pcm.size() - kMaxBufferedPcmFloats;
                    const std::size_t alignedExcess = (excess / kAudioChannels) * kAudioChannels;
                    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(alignedExcess));
                }

                constexpr std::size_t kChunkFloats = kAudioFrameSamples * kAudioChannels;
                std::size_t offset = 0;
                while (pcm.size() - offset >= kChunkFloats && !stopThread.load(std::memory_order_acquire)) {
                    const std::span<const float> chunk(pcm.data() + offset, kChunkFloats);
                    offset += kChunkFloats;
                    const bool silent = isAudioSilent(chunk);
                    if (!silent) {
                        if (inSilence) {
                            encoder.reset();
                            inSilence = false;
                        }
                        trailingSilent = 0;
                    } else if (inSilence) {
                        continue;
                    } else if (trailingSilent < kTrailingSilentFrames) {
                        ++trailingSilent;
                    } else {
                        inSilence = true;
                        trailingSilent = 0;
                        continue;
                    }

                    auto encoded = encoder.encode(chunk);
                    if (!encoded) {
                        continue;
                    }
                    wire::AudioFrame frame;
                    frame.set_data(encoded->data(), encoded->size());
                    frame.set_timestamp_microseconds(nextTimestamp());
                    publishFrame(std::move(frame));
                }
                if (offset > 0) {
                    pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(offset));
                }
                waitInterruptible(capture.deviceChangeHandle(), 10);
            }
            capture.closeEndpoint();
        } catch (...) {
            publishConfig(false);
        }
    }

    void joinThread() {
        stopThread.store(true, std::memory_order_release);
        if (stopEvent) {
            SetEvent(stopEvent);
        }
        if (thread.joinable()) {
            thread.join();
        }
        enabled = false;
        std::lock_guard lock(mutex);
        pendingConfig.reset();
        frames.clear();
    }

    void stopAll() {
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        joinThread();
        wake.first.close();
    }
};

DesktopAudioRuntime::DesktopAudioRuntime(ruvia::event_loop loop) : state_(std::make_shared<State>(std::move(loop))) {}
DesktopAudioRuntime::~DesktopAudioRuntime() { stop(); }

void DesktopAudioRuntime::setEnabled(bool enabled) {
    if (state_->stopped || state_->enabled == enabled) {
        return;
    }
    if (!enabled) {
        state_->joinThread();
        return;
    }
    state_->enabled = true;
    state_->stopThread.store(false, std::memory_order_release);
    if (state_->stopEvent) {
        ResetEvent(state_->stopEvent);
    }
    state_->thread = std::thread([state = state_] { state->runCapture(); });
}

void DesktopAudioRuntime::stop() { state_->stopAll(); }

asio::awaitable<std::optional<DesktopAudioUpdate>> DesktopAudioRuntime::next() {
    for (;;) {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->stopped) {
                co_return std::nullopt;
            }
            if (state_->pendingConfig) {
                DesktopAudioUpdate update;
                update.config = std::exchange(state_->pendingConfig, std::nullopt);
                co_return update;
            }
            if (!state_->frames.empty()) {
                DesktopAudioUpdate update;
                update.frame = std::move(state_->frames.front());
                state_->frames.pop_front();
                co_return update;
            }
        }
        const auto woken = co_await ruvia::as_awaitable(awaitWake(state_->wake.second));
        if (!woken.has_value()) {
            co_return std::nullopt;
        }
    }
}

} // namespace ultraterimal::agent
