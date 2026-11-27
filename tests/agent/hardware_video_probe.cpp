#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <wels/codec_api.h>
#include <wrl/client.h>

#include "features/desktop/codecs.protocol.h"
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
std::string jsonQuote(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char ch : text) {
        if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch >= 0x20u) {
            out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back('"');
    return out;
}
struct AdapterReport {
    bool decoded = false;
    unsigned framesVerified = 0;
    double meanMs = 0;
    double maxMs = 0;
    std::uint64_t outputBytes = 0;
    unsigned hardwareFrames = 0;
    std::string error;
    bool hasHresult = false;
    std::uint32_t hresult = 0;
};
AdapterReport encodeFrames() {
    using namespace ultraterimal::agent;
    AdapterReport report;
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
            report.framesVerified = frame + 1;
            if (encoder.encode(bgra, 1280 * 4, frame * 16667ULL, false)) {
                throw std::runtime_error("Duplicate hardware timestamp accepted");
            }
        }
        report.decoded = true;
        report.meanMs = total / 120;
        report.maxMs = maximum;
        report.outputBytes = bytes;
        report.hardwareFrames = hardwareFrames;
    } catch (const std::system_error& error) {
        report.error = error.what();
        report.hasHresult = true;
        report.hresult = static_cast<std::uint32_t>(error.code().value());
    } catch (const std::exception& error) {
        report.error = error.what();
    }
    return report;
}
std::string inspect(MediaRuntime& runtime, const GUID& codec) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, codec};
    Activations candidates;
    const auto result = runtime.enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                          &input, &output, &candidates.items, &candidates.count);
    std::vector<std::uint32_t> activations;
    std::vector<bool> timeouts;
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
    std::ostringstream out;
    out << "{\"enumeration_hresult\":" << static_cast<std::uint32_t>(result) << ",\"candidates\":" << candidates.count << ",\"activation_hresult\":[";
    for (std::size_t i = 0; i < activations.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << activations[i];
    }
    out << "],\"idle_timeout_checks\":[";
    for (std::size_t i = 0; i < timeouts.size(); ++i) {
        if (i) {
            out << ',';
        }
        out << (timeouts[i] ? "true" : "false");
    }
    out << "]}";
    return out.str();
}
} // namespace
int main() {
    MediaRuntime runtime;
    std::ostringstream out;
    out << "{\"media_started\":" << (runtime.started ? "true" : "false") << ",\"startup_hresult\":" << static_cast<std::uint32_t>(runtime.status);
    if (runtime.started) {
        out << ",\"h264\":" << inspect(runtime, MFVideoFormat_H264) << ",\"h265\":" << inspect(runtime, MFVideoFormat_HEVC);
    }
    const auto adapter = encodeFrames();
    out << ",\"adapter\":{\"decoded\":" << (adapter.decoded ? "true" : "false") << ",\"frames_verified\":" << adapter.framesVerified;
    if (adapter.decoded) {
        out << ",\"mean_encode_decode_ms\":" << adapter.meanMs << ",\"max_encode_decode_ms\":" << adapter.maxMs
            << ",\"output_bytes\":" << adapter.outputBytes << ",\"single_frame_without_drain\":true,\"hardware_frames\":" << adapter.hardwareFrames
            << ",\"software_frames\":" << (120 - adapter.hardwareFrames);
    }
    if (!adapter.error.empty()) {
        out << ",\"error\":" << jsonQuote(adapter.error);
    }
    if (adapter.hasHresult) {
        out << ",\"hresult\":" << adapter.hresult;
    }
    out << "},\"optional_codecs\":[";
    bool firstOptional = true;
    for (const auto preferred : {ultraterimal::wire::HEVC, ultraterimal::wire::AV1}) {
        using namespace ultraterimal;
        std::vector<std::uint8_t> pixels(1280 * 720 * 4, 90);
        agent::VideoEncoder encoder({1280, 720, 8000000, 60, false, {wire::AVC, preferred}});
        const auto started = std::chrono::steady_clock::now();
        const auto frame = encoder.encode(pixels, 1280 * 4, 1, true);
        const auto selected = frame && frame->codec.starts_with("hev1.") ? wire::HEVC : frame && frame->codec.starts_with("av01.") ? wire::AV1
                                                                                                                                   : wire::AVC;
        const auto valid = frame && agent::verifyVideoBaseline(selected, frame->annexB, 1280, 720);
        if (!firstOptional) {
            out << ',';
        }
        firstOptional = false;
        out << "{\"requested\":" << jsonQuote(wire::VideoCodec_Name(preferred)) << ",\"codec\":" << jsonQuote(frame ? frame->codec : "failed")
            << ",\"hardware\":" << (encoder.hardware() ? "true" : "false") << ",\"decoded\":" << (valid ? "true" : "false")
            << ",\"first_frame_ms\":" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() << '}';
        if (!valid) {
            throw std::runtime_error("Optional hardware codec or fallback is not independently decodable");
        }
    }
    out << "],\"agent_hardware_path_implemented\":true}";
    std::cout << out.str() << '\n';
    return adapter.decoded ? 0 : 1;
}
