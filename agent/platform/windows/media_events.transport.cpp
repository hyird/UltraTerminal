#include "media_events.transport.h"

#include <atomic>
#include <limits>
#include <mferror.h>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "handle.types.h"

namespace ultraterimal::agent {
struct MediaEvents::State final : IMFAsyncCallback {
    std::atomic<ULONG> references{1};
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> source;
    Microsoft::WRL::ComPtr<IMFShutdown> shutdown;
    UniqueHandle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    HRESULT result = E_PENDING;
    bool stopped = false;

    explicit State(IMFMediaEventGenerator* generator) : source(generator) {
        if (!generator || !ready || FAILED(source.As(&shutdown))) {
            throw std::runtime_error("Cannot create media event wait");
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFAsyncCallback)) {
            *object = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = --references;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* completion) noexcept override {
        try {
            Microsoft::WRL::ComPtr<IMFMediaEvent> completed;
            const auto status = source->EndGetEvent(completion, &completed);
            {
                const std::lock_guard lock(mutex);
                result = status;
                event = std::move(completed);
            }
            SetEvent(ready.get());
            return S_OK;
        } catch (...) {
            // Never allow a C++ exception to cross the COM callback boundary.
            SetEvent(ready.get());
            return E_FAIL;
        }
    }
};
MediaEvents::MediaEvents(IMFMediaEventGenerator* source) : state_(new State(source)) {
    const auto result = source->BeginGetEvent(state_, nullptr);
    if (FAILED(result)) {
        state_->result = result;
        SetEvent(state_->ready.get());
    }
}
MediaEvents::~MediaEvents() {
    stop();
    state_->Release();
}
void MediaEvents::stop() noexcept {
    if (state_->stopped) {
        return;
    }
    state_->stopped = true;
    state_->shutdown->Shutdown();
    SetEvent(state_->ready.get());
}
std::expected<Microsoft::WRL::ComPtr<IMFMediaEvent>, HRESULT> MediaEvents::next(std::chrono::milliseconds timeout) {
    if (state_->stopped) {
        return std::unexpected(MF_E_SHUTDOWN);
    }
    if (timeout.count() < 0 || timeout.count() >= std::numeric_limits<DWORD>::max()) {
        return std::unexpected(E_INVALIDARG);
    }
    const auto wait = WaitForSingleObject(state_->ready.get(), static_cast<DWORD>(timeout.count()));
    if (wait != WAIT_OBJECT_0) {
        const auto error = wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError());
        stop();
        return std::unexpected(error);
    }
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    HRESULT result;
    {
        const std::lock_guard lock(state_->mutex);
        result = state_->result;
        event = std::move(state_->event);
    }
    if (FAILED(result) || !event) {
        stop();
        return std::unexpected(FAILED(result) ? result : E_UNEXPECTED);
    }
    result = state_->source->BeginGetEvent(state_, nullptr);
    if (FAILED(result)) {
        stop();
        return std::unexpected(result);
    }
    return event;
}
} // namespace ultraterimal::agent
