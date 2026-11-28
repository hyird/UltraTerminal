#include "audio_capture.transport.h"

// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "features/desktop/audio.protocol.h"

namespace ultraterimal::agent {
namespace {

using Microsoft::WRL::ComPtr;

class NotificationClient final : public IMMNotificationClient {
  public:
    explicit NotificationClient(HANDLE changedEvent) noexcept : changedEvent_(changedEvent) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refCount_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (changedEvent_) {
            SetEvent(changedEvent_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && changedEvent_) {
            SetEvent(changedEvent_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

  private:
    std::atomic<ULONG> refCount_{1};
    HANDLE changedEvent_;
};

bool parseWaveFormat(const WAVEFORMATEX* wave, AudioPcmFormat& out) noexcept {
    if (!wave || wave->nSamplesPerSec == 0 || wave->nChannels == 0 || wave->nChannels > 16) {
        return false;
    }
    out.sampleRate = wave->nSamplesPerSec;
    out.channels = wave->nChannels;
    if (wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wave->wBitsPerSample == 32) {
        out.bitsPerSample = 32;
        out.isFloat = true;
        return true;
    }
    if (wave->wFormatTag == WAVE_FORMAT_PCM && wave->wBitsPerSample == 16) {
        out.bitsPerSample = 16;
        out.isFloat = false;
        return true;
    }
    if (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wave->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wave->wBitsPerSample == 32) {
            out.bitsPerSample = 32;
            out.isFloat = true;
            return true;
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && wave->wBitsPerSample == 16) {
            out.bitsPerSample = 16;
            out.isFloat = false;
            return true;
        }
    }
    return false;
}

} // namespace

struct WindowsAudioCapture::State {
    HANDLE changedEvent = nullptr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<NotificationClient> notifications;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    AudioPcmFormat format{};
    bool started = false;

    State() {
        changedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ensureEnumerator();
    }

    ~State() {
        closeEndpoint();
        if (enumerator && notifications) {
            (void)enumerator->UnregisterEndpointNotificationCallback(notifications.Get());
        }
        notifications.Reset();
        enumerator.Reset();
        if (changedEvent) {
            CloseHandle(changedEvent);
            changedEvent = nullptr;
        }
    }

    bool ensureEnumerator() {
        if (enumerator) {
            return true;
        }
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || !enumerator) {
            enumerator.Reset();
            return false;
        }
        if (changedEvent) {
            notifications.Attach(new NotificationClient(changedEvent));
            if (FAILED(enumerator->RegisterEndpointNotificationCallback(notifications.Get()))) {
                notifications.Reset();
            }
        }
        return true;
    }

    void closeEndpoint() noexcept {
        if (client && started) {
            (void)client->Stop();
        }
        started = false;
        capture.Reset();
        client.Reset();
        device.Reset();
    }
};

WindowsAudioCapture::WindowsAudioCapture() : state_(std::make_unique<State>()) {}
WindowsAudioCapture::~WindowsAudioCapture() = default;

bool WindowsAudioCapture::openDefaultEndpoint() {
    closeEndpoint();
    if (!state_->ensureEnumerator()) {
        return false;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(state_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
        return false;
    }
    ComPtr<IAudioClient> client;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) || !client) {
        return false;
    }

    WAVEFORMATEXTENSIBLE desired{};
    desired.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    desired.Format.nChannels = static_cast<WORD>(kAudioChannels);
    desired.Format.nSamplesPerSec = kAudioSampleRate;
    desired.Format.wBitsPerSample = 32;
    desired.Format.nBlockAlign = static_cast<WORD>(kAudioChannels * 4);
    desired.Format.nAvgBytesPerSec = kAudioSampleRate * desired.Format.nBlockAlign;
    desired.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    desired.Samples.wValidBitsPerSample = 32;
    desired.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    desired.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    constexpr REFERENCE_TIME kBufferDuration = 400000; // 40 ms
    AudioPcmFormat format{};
    HRESULT hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kBufferDuration, 0, &desired.Format, nullptr);
    if (SUCCEEDED(hr)) {
        format = {kAudioSampleRate, static_cast<std::uint16_t>(kAudioChannels), 32, true};
    } else {
        client.Reset();
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) || !client) {
            return false;
        }
        WAVEFORMATEX* mixFormat = nullptr;
        if (FAILED(client->GetMixFormat(&mixFormat)) || !mixFormat) {
            return false;
        }
        struct MixGuard {
            WAVEFORMATEX* ptr;
            ~MixGuard() { CoTaskMemFree(ptr); }
        } mixGuard{mixFormat};
        if (!parseWaveFormat(mixFormat, format)) {
            return false;
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration, 0, mixFormat, nullptr);
        if (FAILED(hr)) {
            return false;
        }
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture))) || !capture) {
        return false;
    }
    if (FAILED(client->Start())) {
        return false;
    }
    state_->device = std::move(device);
    state_->client = std::move(client);
    state_->capture = std::move(capture);
    state_->format = format;
    state_->started = true;
    return true;
}

bool WindowsAudioCapture::readAvailable(std::vector<float>& outStereo48k) {
    if (!state_->capture) {
        return false;
    }
    for (;;) {
        UINT32 packetFrames = 0;
        HRESULT hr = state_->capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) {
            return false;
        }
        if (packetFrames == 0) {
            return true;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = state_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            return true;
        }
        if (FAILED(hr)) {
            return false;
        }
        struct ReleaseGuard {
            IAudioCaptureClient* capture;
            UINT32 frames;
            ~ReleaseGuard() { (void)capture->ReleaseBuffer(frames); }
        } guard{state_->capture.Get(), frames};
        if (frames > 0) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                const std::size_t outFrames = state_->format.sampleRate == kAudioSampleRate
                                                  ? frames
                                                  : static_cast<std::size_t>((static_cast<std::uint64_t>(frames) * kAudioSampleRate + state_->format.sampleRate - 1) / state_->format.sampleRate);
                outStereo48k.insert(outStereo48k.end(), outFrames * kAudioChannels, 0.0f);
            } else {
                const std::size_t frameStride = static_cast<std::size_t>(state_->format.channels) * (state_->format.bitsPerSample / 8);
                const std::size_t bytes = static_cast<std::size_t>(frames) * frameStride;
                if (!convertPcmToStereo48kFloat({data, bytes}, state_->format, outStereo48k)) {
                    return false;
                }
            }
        }
    }
}

void WindowsAudioCapture::closeEndpoint() noexcept { state_->closeEndpoint(); }
bool WindowsAudioCapture::isOpen() const noexcept { return state_->capture != nullptr; }
void* WindowsAudioCapture::deviceChangeHandle() const noexcept { return state_->changedEvent; }
void WindowsAudioCapture::resetDeviceChangeEvent() noexcept {
    if (state_->changedEvent) {
        ResetEvent(state_->changedEvent);
    }
}

} // namespace ultraterimal::agent
