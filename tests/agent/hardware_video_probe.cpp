#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <boost/json.hpp>
#include <wels/codec_api.h>
#include <wrl/client.h>

#include "features/desktop/video.transport.h"
#include "features/desktop/video_color.protocol.h"
#include "platform/windows/media_encoder.transport.h"
#include "platform/windows/media_events.transport.h"
#include "platform/windows/media_runtime.transport.h"

namespace {
using ultraterimal::agent::MediaRuntime;
struct Activations {
    IMFActivate** items = nullptr;
    UINT32 count = 0;
    ~Activations() {
        for (UINT32 index = 0; index < count; ++index) {
            items[index]->Release();
        }
        CoTaskMemFree(items);
    }
};
boost::json::object encodeFrames() {
    using namespace ultraterimal::agent;
    boost::json::object report{{"decoded", false}};
    try {
        VideoEncoder encoder({1280, 720, 4000000, 60});
        unsigned hardwareFrames = 0;
        ISVCDecoder* raw = nullptr;
        if (WelsCreateDecoder(&raw) != 0 || !raw) {
            throw std::runtime_error("Create independent decoder");
        }
        struct Decoder {
            ISVCDecoder* value;
            ~Decoder() {
                value->Uninitialize();
                WelsDestroyDecoder(value);
            }
        } decoder{raw};
        SDecodingParam settings{};
        settings.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
        if (raw->Initialize(&settings) != 0) {
            throw std::runtime_error("Initialize independent decoder");
        }
        std::vector<std::uint8_t> bgra(1280 * 720 * 4), nv12(1280 * 720 * 3 / 2);
        double total = 0, maximum = 0;
        std::uint64_t bytes = 0;
        if (encoder.encode({}, 1280 * 4, 0, true)) {
            throw std::runtime_error("Invalid hardware input accepted");
        }
        for (unsigned frame = 0; frame < 120; ++frame) {
            const auto gray = static_cast<std::uint8_t>(32 + frame % 160);
            std::fill(bgra.begin(), bgra.end(), gray);
            if (!convertVideoPixels(bgra, 1280 * 4, 1280, 720, VideoPixelFormat::kNv12, nv12)) {
                throw std::runtime_error("Convert hardware pixels");
            }
            if (frame == 40 || frame == 80) {
                encoder.setRate(frame == 40 ? 30 : 60, frame == 40 ? 2000000 : 4000000);
                encoder.setDetail(frame == 40);
            }
            const auto started = std::chrono::steady_clock::now();
            const bool key = frame % 30 == 0;
            auto encoded = encoder.encode(bgra, 1280 * 4, frame * 16667ULL, key);
            if (!encoded) {
                throw std::runtime_error("Production video adapter failed");
            }
            unsigned char* planes[3]{};
            SBufferInfo decoded{};
            const auto result = raw->DecodeFrameNoDelay(encoded->annexB.data(), static_cast<int>(encoded->annexB.size()), planes, &decoded);
            if (result != dsErrorFree || decoded.iBufferStatus != 1 || decoded.UsrData.sSystemBuffer.iWidth != 1280 ||
                decoded.UsrData.sSystemBuffer.iHeight != 720 || encoded->timestampMicroseconds != frame * 16667ULL || (key && !encoded->keyFrame)) {
                throw std::runtime_error("Hardware output geometry, keyframe or timestamp invalid");
            }
            for (int plane = 0; plane < 3; ++plane) {
                if (!planes[plane]) {
                    throw std::runtime_error("Missing decoded plane");
                }
                const int width = plane ? 640 : 1280, height = plane ? 360 : 720;
                const int expected = plane ? 128 : nv12[0];
                const int stride = decoded.UsrData.sSystemBuffer.iStride[plane ? 1 : 0];
                for (int y = 0; y < height; y += 37) {
                    for (int x = 0; x < width; x += 53) {
                        if (std::abs(int(planes[plane][y * stride + x]) - expected) > 4) {
                            throw std::runtime_error("Hardware output color or frame order invalid");
                        }
                    }
                }
            }
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            total += elapsed;
            maximum = std::max(maximum, elapsed);
            bytes += encoded->annexB.size();
            hardwareFrames += encoder.hardware() ? 1 : 0;
            report["frames_verified"] = frame + 1;
            if (encoder.encode(bgra, 1280 * 4, frame * 16667ULL, false)) {
                throw std::runtime_error("Duplicate hardware timestamp accepted");
            }
        }
        report["decoded"] = true;
        report["mean_encode_decode_ms"] = total / 120;
        report["max_encode_decode_ms"] = maximum;
        report["output_bytes"] = bytes;
        report["single_frame_without_drain"] = true;
        report["hardware_frames"] = hardwareFrames;
        report["software_frames"] = 120 - hardwareFrames;
    } catch (const std::system_error& error) {
        report["error"] = error.what();
        report["hresult"] = static_cast<std::uint32_t>(error.code().value());
    } catch (const std::exception& error) {
        report["error"] = error.what();
    }
    return report;
}
boost::json::object inspect(MediaRuntime& runtime, const GUID& codec) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, codec};
    Activations candidates;
    const auto result = runtime.enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &input, &output, &candidates.items, &candidates.count);
    boost::json::array activations;

    boost::json::array timeouts;
    if (SUCCEEDED(result)) {
        for (UINT32 index = 0; index < candidates.count; ++index) {
            Microsoft::WRL::ComPtr<IMFTransform> transform;
            const auto activated = candidates.items[index]->ActivateObject(IID_PPV_ARGS(&transform));
            activations.push_back(static_cast<std::uint32_t>(activated));
            transform.Reset();
            candidates.items[index]->ShutdownObject();
            if (SUCCEEDED(activated) && codec == MFVideoFormat_H264) {
                bool timeoutVerified = false;
                if (SUCCEEDED(candidates.items[index]->ActivateObject(IID_PPV_ARGS(&transform)))) {
                    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> idle;
                    if (SUCCEEDED(transform.As(&idle))) {
                        ultraterimal::agent::MediaEvents waiting(idle.Get());
                        const auto expired = waiting.next(std::chrono::milliseconds(10));
                        const auto closed = waiting.next(std::chrono::milliseconds(0));
                        timeoutVerified = !expired && expired.error() == HRESULT_FROM_WIN32(ERROR_TIMEOUT) &&
                                          !closed && closed.error() == MF_E_SHUTDOWN;
                    }
                }
                timeouts.push_back(timeoutVerified);
                transform.Reset();
                candidates.items[index]->ShutdownObject();
            }
        }
    }
    return {{"enumeration_hresult", static_cast<std::uint32_t>(result)}, {"candidates", candidates.count}, {"activation_hresult", std::move(activations)}, {"idle_timeout_checks", std::move(timeouts)}};
}
} // namespace
int main() {
    MediaRuntime runtime;
    boost::json::object report{{"media_started", runtime.started}, {"startup_hresult", static_cast<std::uint32_t>(runtime.status)}};
    if (runtime.started) {
        report["h264"] = inspect(runtime, MFVideoFormat_H264);
        report["h265"] = inspect(runtime, MFVideoFormat_HEVC);
    }
    report["adapter"] = encodeFrames();
    report["agent_hardware_path_implemented"] = true;
    std::cout << boost::json::serialize(report) << '\n';
    return report.at("adapter").as_object().at("decoded").as_bool() ? 0 : 1;
}
