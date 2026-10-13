#include "media_encoder.transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <codecapi.h>
#include <cstring>
#include <limits>
#include <mferror.h>
#include <mftransform.h>
#include <optional>
#include <stdexcept>
#include <strmif.h>
#include <system_error>

#include <d3d10_1.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "media_events.transport.h"
#include "media_runtime.transport.h"

namespace ultraterimal::agent {
namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT result) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), "Hardware video operation");
    }
}
HRESULT codecValue(ICodecAPI& codec, const GUID& property, ULONG value) {
    VARIANT setting{};
    setting.vt = VT_UI4;
    setting.ulVal = value;
    return codec.SetValue(&property, &setting);
}
// MF_MT_FRAME_RATE is fixed while streaming and the MFT budgets bytes per
// nominal picture. Scale its mean so every actual picture keeps bitrate / FPS.
ULONG pictureBitrate(std::uint32_t bitrate, std::uint32_t nominalFramesPerSecond, std::uint32_t framesPerSecond) {
    return static_cast<ULONG>(std::clamp<std::uint64_t>(static_cast<std::uint64_t>(bitrate) * nominalFramesPerSecond / framesPerSecond, 64000, 100000000));
}
// MF owns callbacks until tracked input samples are released. No callback
// touches the encoder or D3D context; only an independently owned atomic slot.
class SurfaceRelease final : public IMFAsyncCallback {
  public:
    explicit SurfaceRelease(std::shared_ptr<std::atomic_bool> available) : available_(std::move(available)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) noexcept override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFAsyncCallback)) {
            return E_NOINTERFACE;
        }
        *output = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const auto remaining = --references_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult*) noexcept override {
        available_->store(true, std::memory_order_release);
        return S_OK;
    }

  private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<std::atomic_bool> available_;
};
} // namespace
struct MediaEncoder::State {
    MediaRuntime runtime;
    MediaEncoderConfig config;
    std::uint32_t nominalFramesPerSecond = config.framesPerSecond;
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> codec;
    std::unique_ptr<MediaEvents> events;
    DWORD inputId = 0, outputId = 0;
    unsigned inputCredits = 0;
    std::optional<std::uint64_t> lastTimestamp;
    bool failed = false;
    bool refreshRequired = true;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> processorEnumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    struct Surface {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorOutputView> view;
        std::shared_ptr<std::atomic_bool> available = std::make_shared<std::atomic_bool>(true);
        ComPtr<IMFAsyncCallback> released;
    };
    std::array<Surface, 4> surfaces;

    explicit State(MediaEncoderConfig settings, ID3D11Device* graphicsDevice) : config(settings), device(graphicsDevice) {
        if (settings.width < 16 || settings.height < 16 || settings.width % 2 || settings.height % 2 ||
            settings.width > 4096 || settings.height > 4096 || static_cast<std::uint64_t>(settings.width) * settings.height > 4096 * 2160 || !settings.framesPerSecond || settings.framesPerSecond > 120 ||
            settings.bitrate < 64000 || settings.bitrate > 100000000) {
            throw std::invalid_argument("Invalid hardware encoder configuration");
        }
        require(runtime.status);
    }
    ~State() {
        if (events) {
            events->stop();
            events.reset();
        }
        codec.Reset();
        transform.Reset();
        if (activation) {
            activation->ShutdownObject();
        }
    }
    void initialize() {
        const GUID format = config.codec == wire::HEVC ? MFVideoFormat_HEVC : config.codec == wire::AV1 ? MFVideoFormat_AV1
                                                                                                        : MFVideoFormat_H264;
        MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, format};
        struct Candidates {
            IMFActivate** items = nullptr;
            UINT32 count = 0;
            ~Candidates() {
                for (UINT32 index = 0; index < count; ++index) {
                    items[index]->Release();
                }
                CoTaskMemFree(items);
            }
        } candidates;
        require(runtime.enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                  &inputInfo, &outputInfo, &candidates.items, &candidates.count));
        for (UINT32 index = 0; index < candidates.count; ++index) {
            if (SUCCEEDED(candidates.items[index]->ActivateObject(IID_PPV_ARGS(&transform)))) {
                activation = candidates.items[index];
                break;
            }
        }
        if (!transform) {
            throw std::runtime_error("No compatible hardware encoder available");
        }
        ComPtr<IMFAttributes> attributes;
        require(transform->GetAttributes(&attributes));
        require(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        require(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
        if (device) {
            UINT32 aware = 0;
            require(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware));
            if (!aware || !runtime.prepareVideoSamples()) {
                require(E_NOTIMPL);
            }
            UINT token = 0;
            require(runtime.createDeviceManager(&token, &deviceManager));
            require(deviceManager->ResetDevice(device.Get(), token));
            ComPtr<ID3D10Multithread> multithread;
            require(device.As(&multithread));
            multithread->SetMultithreadProtected(TRUE);
            require(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager.Get())));
            initializeGpu();
        }
        require(transform.As(&codec));
        require(codec->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame));
        if (SUCCEEDED(codec->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount))) {
            require(codecValue(*codec.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0));
        }
        if (config.sharp) {
            // MaxQP is static: configure before streaming, recreate on changes.
            require(codecValue(*codec.Get(), CODECAPI_AVEncVideoMaxQP, 18));
        }
        DWORD inputs = 0, outputs = 0;
        require(transform->GetStreamCount(&inputs, &outputs));
        if (inputs != 1 || outputs != 1) {
            throw std::runtime_error("Unsupported hardware stream topology");
        }
        const auto ids = transform->GetStreamIDs(1, &inputId, 1, &outputId);
        if (ids != E_NOTIMPL) {
            require(ids);
        }
        ComPtr<IMFMediaType> output, input;
        require(runtime.createType(&output));
        require(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        require(output->SetGUID(MF_MT_SUBTYPE, format));
        require(output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate));
        const auto profile = config.codec == wire::HEVC ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8) : config.codec == wire::AV1 ? 0u
                                                                                                                                         : static_cast<UINT32>(eAVEncH264VProfile_Base);
        require(output->SetUINT32(MF_MT_MPEG2_PROFILE, profile));
        require(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        require(output->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
        require(output->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601));
        require(output->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
        require(output->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB));
        require(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, config.width, config.height));
        require(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, config.framesPerSecond, 1));
        require(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
        require(transform->SetOutputType(outputId, output.Get(), 0));
        require(runtime.createType(&input));
        require(output->CopyAllItems(input.Get()));
        require(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
        require(input->DeleteItem(MF_MT_AVG_BITRATE));
        require(input->DeleteItem(MF_MT_MPEG2_PROFILE));
        require(transform->SetInputType(inputId, input.Get(), 0));
        ComPtr<IMFMediaEventGenerator> source;
        require(transform.As(&source));
        events = std::make_unique<MediaEvents>(source.Get());
        require(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        require(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }
    void initializeGpu() {
        require(device.As(&videoDevice));
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        require(context.As(&videoContext));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = content.OutputFrameRate = {config.framesPerSecond, 1};
        content.InputWidth = content.OutputWidth = config.width;
        content.InputHeight = content.OutputHeight = config.height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        require(videoDevice->CreateVideoProcessorEnumerator(&content, &processorEnumerator));
        UINT inputSupport = 0, outputSupport = 0;
        require(processorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &inputSupport));
        require(processorEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &outputSupport));
        if (!(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            require(E_NOTIMPL);
        }
        require(videoDevice->CreateVideoProcessor(processorEnumerator.Get(), 0, &processor));
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{}, outputColor{};
        inputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        // Full RGB -> BT.601 limited NV12, matching the CPU and stream metadata.
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &inputColor);
        videoContext->VideoProcessorSetOutputColorSpace(processor.Get(), &outputColor);
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = config.width;
        description.Height = config.height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        for (auto& surface : surfaces) {
            require(device->CreateTexture2D(&description, nullptr, &surface.texture));
            require(videoDevice->CreateVideoProcessorOutputView(surface.texture.Get(), processorEnumerator.Get(), &view, &surface.view));
            auto callback = std::make_unique<SurfaceRelease>(surface.available);
            surface.released.Attach(callback.release());
        }
    }
    MediaEventType next(std::chrono::steady_clock::time_point deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            require(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        auto received = events->next(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        require(received ? S_OK : received.error());
        HRESULT status = S_OK;
        require((*received)->GetStatus(&status));
        require(status);
        MediaEventType type;
        require((*received)->GetType(&type));
        if (type == METransformNeedInput) {
            if (++inputCredits > 16) {
                require(E_UNEXPECTED);
            }
        }
        return type;
    }
};
MediaEncoder::MediaEncoder(MediaEncoderConfig config, ID3D11Device* device) : state_(std::make_unique<State>(config, device)) { state_->initialize(); }
MediaEncoder::~MediaEncoder() = default;
bool MediaEncoder::gpu() const { return state_->device != nullptr; }
std::expected<void, HRESULT> MediaEncoder::setDetail(bool sharp) {
    auto& state = *state_;
    if (state.failed) {
        return std::unexpected(E_UNEXPECTED);
    }
    if (state.config.sharp == sharp) {
        return {};
    }
    try {
        auto config = state.config;
        config.sharp = sharp;
        MediaEncoder replacement(config, state.device.Get());
        replacement.state_->lastTimestamp = state.lastTimestamp;
        state_.swap(replacement.state_);
        return {};
    } catch (const std::system_error& error) {
        state.failed = true;
        return std::unexpected(static_cast<HRESULT>(error.code().value()));
    } catch (const std::runtime_error&) {
        state.failed = true;
        return std::unexpected(E_FAIL);
    }
}
std::expected<void, HRESULT> MediaEncoder::setRate(std::uint32_t framesPerSecond, std::uint32_t bitrate) {
    auto& state = *state_;
    if (state.failed || !framesPerSecond || framesPerSecond > 120 || bitrate < 64000 || bitrate > 100000000) {
        return std::unexpected(E_INVALIDARG);
    }
    const auto mean = pictureBitrate(bitrate, state.nominalFramesPerSecond, framesPerSecond);
    if (mean != pictureBitrate(state.config.bitrate, state.nominalFramesPerSecond, state.config.framesPerSecond)) {
        const auto result = codecValue(*state.codec.Get(), CODECAPI_AVEncCommonMeanBitRate, mean);
        if (FAILED(result)) {
            state.failed = true;
            return std::unexpected(result);
        }
    }
    state.config.framesPerSecond = framesPerSecond;
    state.config.bitrate = bitrate;
    return {};
}
std::expected<MediaEncodedFrame, HRESULT> MediaEncoder::encode(std::span<const std::uint8_t> nv12, std::uint64_t timestampMicroseconds, bool forceKeyFrame) {
    if (nv12.size() != static_cast<std::size_t>(state_->config.width) * state_->config.height * 3 / 2 || gpu()) {
        return std::unexpected(E_INVALIDARG);
    }
    ComPtr<IMFMediaBuffer> memory;
    const auto size = static_cast<DWORD>(nv12.size());
    auto status = state_->runtime.createBuffer(size, &memory);
    BYTE* data = nullptr;
    if (SUCCEEDED(status)) {
        status = memory->Lock(&data, nullptr, nullptr);
    }
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    std::memcpy(data, nv12.data(), size);
    status = memory->Unlock();
    if (SUCCEEDED(status)) {
        status = memory->SetCurrentLength(size);
    }
    if (FAILED(status)) {
        return std::unexpected(status);
    }
    return encodeBuffer(memory.Get(), timestampMicroseconds, forceKeyFrame);
}
std::expected<MediaEncodedFrame, HRESULT> MediaEncoder::encodeTexture(ID3D11Texture2D* bgra, std::uint64_t timestampMicroseconds, bool forceKeyFrame) {
    auto& state = *state_;
    if (!bgra || !gpu() || state.failed || timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) / 10 ||
        (state.lastTimestamp && timestampMicroseconds <= *state.lastTimestamp)) {
        return std::unexpected(E_INVALIDARG);
    }
    D3D11_TEXTURE2D_DESC description{};
    bgra->GetDesc(&description);
    ComPtr<ID3D11Device> sourceDevice;
    bgra->GetDevice(&sourceDevice);
    if (description.Width != state.config.width || description.Height != state.config.height || description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || sourceDevice.Get() != state.device.Get()) {
        return std::unexpected(E_INVALIDARG);
    }
    for (auto& surface : state.surfaces) {
        if (!surface.available->exchange(false, std::memory_order_acquire)) {
            continue;
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
        inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> view;
        auto status = state.videoDevice->CreateVideoProcessorInputView(bgra, state.processorEnumerator.Get(), &inputDescription, &view);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = view.Get();
        if (SUCCEEDED(status)) {
            status = state.videoContext->VideoProcessorBlt(state.processor.Get(), surface.view.Get(), 0, 1, &stream);
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(status)) {
            status = state.runtime.createSurfaceBuffer(__uuidof(ID3D11Texture2D), surface.texture.Get(), 0, FALSE, &buffer);
        }
        if (SUCCEEDED(status)) {
            status = buffer->SetCurrentLength(state.config.width * state.config.height * 3 / 2);
        }
        if (FAILED(status)) {
            surface.available->store(true, std::memory_order_release);
            return std::unexpected(status);
        }
        return encodeBuffer(buffer.Get(), timestampMicroseconds, forceKeyFrame, surface.released.Get());
    }
    return std::unexpected(MF_E_NOTACCEPTING); // Bounded pool, never overwrite a live MFT input.
}
std::expected<MediaEncodedFrame, HRESULT> MediaEncoder::encodeBuffer(IMFMediaBuffer* buffer, std::uint64_t timestampMicroseconds, bool forceKeyFrame, IMFAsyncCallback* released) {
    auto& state = *state_;
    if (state.failed || !buffer || timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) / 10 ||
        (state.lastTimestamp && timestampMicroseconds <= *state.lastTimestamp)) {
        return std::unexpected(E_INVALIDARG);
    }
    try {
        forceKeyFrame = forceKeyFrame || state.refreshRequired;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!state.inputCredits) {
            if (state.next(deadline) == METransformHaveOutput) {
                require(E_UNEXPECTED);
            }
        }
        if (forceKeyFrame) {
            require(codecValue(*state.codec.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1));
        }
        ComPtr<IMFSample> input;
        require(released ? state.runtime.createVideoSample(nullptr, &input) : state.runtime.createSample(&input));
        if (released) {
            ComPtr<IMFTrackedSample> tracked;
            require(input.As(&tracked));
            require(tracked->SetAllocator(released, nullptr));
        }
        BYTE* data = nullptr;
        require(input->AddBuffer(buffer));
        require(input->SetSampleTime(static_cast<LONGLONG>(timestampMicroseconds * 10)));
        require(input->SetSampleDuration(10000000 / state.config.framesPerSecond));
        require(state.transform->ProcessInput(state.inputId, input.Get(), 0));
        --state.inputCredits;
        state.lastTimestamp = timestampMicroseconds;
        while (state.next(deadline) != METransformHaveOutput) {
        }
        MFT_OUTPUT_STREAM_INFO info{};
        require(state.transform->GetOutputStreamInfo(state.outputId, &info));
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            require(E_NOTIMPL);
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = state.outputId;
        DWORD status = 0;
        const auto produced = state.transform->ProcessOutput(0, 1, &output, &status);
        ComPtr<IMFSample> sample;
        sample.Attach(output.pSample);
        ComPtr<IMFCollection> events;
        events.Attach(output.pEvents);
        require(produced);
        if (!sample) {
            require(E_UNEXPECTED);
        }
        LONGLONG sampleTime = 0;
        require(sample->GetSampleTime(&sampleTime));
        if (sampleTime != static_cast<LONGLONG>(timestampMicroseconds * 10)) {
            require(E_UNEXPECTED); // No reordered or delayed frame may masquerade as current pixels.
        }
        ComPtr<IMFMediaBuffer> encoded;
        require(sample->ConvertToContiguousBuffer(&encoded));
        DWORD length = 0;
        require(encoded->GetCurrentLength(&length));
        if (!length || length > 16 * 1024 * 1024) {
            require(E_UNEXPECTED);
        }
        MediaEncodedFrame frame{{}, timestampMicroseconds, false};
        frame.bytes.resize(length);
        require(encoded->Lock(&data, nullptr, nullptr));
        std::memcpy(frame.bytes.data(), data, length);
        require(encoded->Unlock());
        UINT32 clean = 0;
        sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
        frame.keyFrame = clean != 0;
        if (forceKeyFrame && !frame.keyFrame) {
            require(E_UNEXPECTED);
        }
        state.refreshRequired = false;
        return frame;
    } catch (const std::system_error& error) {
        state.failed = true;
        state.events->stop();
        return std::unexpected(static_cast<HRESULT>(error.code().value()));
    }
}
} // namespace ultraterimal::agent
