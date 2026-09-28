#include "media_encoder.transport.h"

#include <chrono>
#include <codecapi.h>
#include <cstring>
#include <icodecapi.h>
#include <limits>
#include <mferror.h>
#include <mftransform.h>
#include <optional>
#include <stdexcept>
#include <system_error>

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
} // namespace
struct MediaEncoder::State {
    MediaRuntime runtime;
    MediaEncoderConfig config;
    ComPtr<IMFActivate> activation;
    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> codec;
    std::unique_ptr<MediaEvents> events;
    DWORD inputId = 0, outputId = 0;
    unsigned inputCredits = 0;
    std::optional<std::uint64_t> lastTimestamp;
    bool failed = false;
    bool refreshRequired = true;

    explicit State(MediaEncoderConfig settings) : config(settings) {
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
        MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_H264};
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
            throw std::runtime_error("No H264 hardware encoder available");
        }
        ComPtr<IMFAttributes> attributes;
        require(transform->GetAttributes(&attributes));
        require(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        require(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
        require(transform.As(&codec));
        require(codec->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame));
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
        require(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
        require(output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate));
        require(output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
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
MediaEncoder::MediaEncoder(MediaEncoderConfig config) : state_(std::make_unique<State>(config)) { state_->initialize(); }
MediaEncoder::~MediaEncoder() = default;
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
        MediaEncoder replacement(config);
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
    if (bitrate != state.config.bitrate) {
        const auto result = codecValue(*state.codec.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
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
    auto& state = *state_;
    const auto size = static_cast<DWORD>(state.config.width * state.config.height * 3 / 2);
    if (state.failed || nv12.size() != size || timestampMicroseconds > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()) / 10 ||
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
        ComPtr<IMFMediaBuffer> memory;
        require(state.runtime.createSample(&input));
        require(state.runtime.createBuffer(size, &memory));
        BYTE* data = nullptr;
        require(memory->Lock(&data, nullptr, nullptr));
        std::memcpy(data, nv12.data(), size);
        require(memory->Unlock());
        require(memory->SetCurrentLength(size));
        require(input->AddBuffer(memory.Get()));
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
