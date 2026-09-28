#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <wels/codec_api.h>

#include "capture.fixture.h"
#include "features/desktop/software_video.transport.h"
#include "features/desktop/video.transport.h"
#include "features/desktop/video_color.protocol.h"
#include "platform/windows/capture.transport.h"

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t cpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("Cannot measure process CPU time");
    }
    const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
void measureCapturePipeline() {
    using namespace ultraterimal;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    test::TestSurface surface;
    agent::DesktopCapture capture;
    std::unique_ptr<agent::SoftwareVideoEncoder> encoder;
    std::vector<std::uint8_t> pixels;
    std::vector<double> painting, capturing, encoding;
    constexpr unsigned warmup = 10, samples = 180;
    std::uint32_t width = 0, height = 0;
    std::uint64_t bytes = 0;
    unsigned captureCalls = 0;
    for (unsigned index = 0; index < warmup + samples; ++index) {
        const auto color = index % 2 ? RGB(208, 32, 32) : RGB(32, 208, 32);
        const auto started = Clock::now();
        surface.paint(color);
        const auto painted = Clock::now();
        std::optional<agent::CapturedFrame> frame;
        const auto deadline = painted + std::chrono::seconds(2);
        while (Clock::now() < deadline) {
            auto candidate = capture.nextFrame(std::chrono::milliseconds(8), &pixels);
            ++captureCalls;
            if (!candidate || candidate->bgra.empty()) {
                continue;
            }
            if (candidate->width <= 64 || candidate->height <= 64) {
                throw std::runtime_error("Capture benchmark display is too small");
            }
            const auto offset = (static_cast<std::size_t>(64) * candidate->width + 64) * 4;
            if (candidate->bgra[offset] == GetBValue(color) && candidate->bgra[offset + 1] == GetGValue(color) && candidate->bgra[offset + 2] == GetRValue(color)) {
                frame = std::move(candidate);
                break;
            }
            pixels = std::move(candidate->bgra);
        }
        if (!frame) {
            throw std::runtime_error("Capture benchmark did not observe its new test pixels");
        }
        const auto captured = Clock::now();
        if (!encoder) {
            width = frame->width;
            height = frame->height;
            encoder = std::make_unique<agent::SoftwareVideoEncoder>(agent::VideoEncoderConfig{width, height, 8000000, 60});
        }
        if (frame->width != width || frame->height != height) {
            throw std::runtime_error("Capture benchmark display changed dimensions");
        }
        const auto encodeStarted = Clock::now();
        const auto encoded = encoder->encode(frame->bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(index + 1) * 16667, index == 0);
        const auto encodedAt = Clock::now();
        if (!encoded || encoded->annexB.empty()) {
            throw std::runtime_error("Capture benchmark encoding failed or skipped a frame");
        }
        pixels = std::move(frame->bgra);
        if (index >= warmup) {
            painting.push_back(std::chrono::duration<double, std::milli>(painted - started).count());
            capturing.push_back(std::chrono::duration<double, std::milli>(captured - painted).count());
            encoding.push_back(std::chrono::duration<double, std::milli>(encodedAt - encodeStarted).count());
            bytes += encoded->annexB.size();
        }
    }
    const auto report = [&](std::string_view stage, std::vector<double> values) {
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        std::sort(values.begin(), values.end());
        std::cout << "{\"workload\":\"real_desktop_64px_patch\",\"stage\":\"" << stage << "\",\"width\":" << width
                  << ",\"height\":" << height << ",\"samples\":" << samples << ",\"mean_ms\":" << mean
                  << ",\"p95_ms\":" << values[samples * 95 / 100 - 1] << ",\"p99_ms\":" << values[samples * 99 / 100 - 1]
                  << ",\"verified_frames_including_warmup\":" << warmup + samples << ",\"capture_calls_including_warmup\":" << captureCalls
                  << ",\"encoded_bytes_per_frame\":" << bytes / samples << "}\n";
    };
    report("test_paint_and_compositor_wait", std::move(painting));
    report("capture_until_verified_pixels", std::move(capturing));
    report("conversion_and_encode", std::move(encoding));
}
template <typename Encoder = ultraterimal::agent::SoftwareVideoEncoder>
void measure(std::uint32_t width, std::uint32_t height, bool scrolling, bool sharp = false) {
    constexpr unsigned warmup = 10, samples = 180;
    const auto stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(stride * height);
    Encoder encoder({width, height, 8000000, 60, sharp});
    ISVCDecoder* decoder = nullptr;
    if (WelsCreateDecoder(&decoder) != 0 || !decoder) {
        throw std::runtime_error("Cannot create benchmark decoder");
    }
    struct DecoderOwner {
        ISVCDecoder* value;
        ~DecoderOwner() {
            value->Uninitialize();
            WelsDestroyDecoder(value);
        }
    } owner{decoder};
    SDecodingParam configuration{};
    configuration.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    configuration.eEcActiveIdc = ERROR_CON_DISABLE;
    if (decoder->Initialize(&configuration) != 0) {
        throw std::runtime_error("Cannot initialize benchmark decoder");
    }
    std::vector<double> elapsed;
    elapsed.reserve(samples);
    std::uint64_t bytes = 0, cpu = 0;
    double squaredError = 0;
    for (unsigned frame = 0; frame < samples + warmup; ++frame) {
        // Synthetic desktop-like rows plus a moving window. Generation and
        // independent decoding are excluded from the encoder timing samples.
        const auto left = frame * 13 % (width - 320);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto row = y + (scrolling ? frame * 7 : 0);
                const bool text = row % 24 < 3 && x % 180 < 130;
                const bool window = x >= left && x < left + 320 && y >= 100 && y < 340;
                auto* pixel = pixels.data() + y * stride + x * 4;
                pixel[0] = window ? 180 : text ? 30
                                               : 240;
                pixel[1] = window ? 90 : text ? 30
                                              : 240;
                pixel[2] = window ? 40 : text ? 30
                                              : 240;
                pixel[3] = 255;
            }
        }
        const auto cpuStart = cpuTicks();
        const auto started = Clock::now();
        const auto encoded = encoder.encode(pixels, stride, frame * 16667ull);
        const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        const auto cpuElapsed = cpuTicks() - cpuStart;
        if (!encoded || encoded->annexB.empty()) {
            throw std::runtime_error("Benchmark dropped or failed to encode a frame");
        }
        unsigned char* planes[3]{};
        SBufferInfo information{};
        const auto status = decoder->DecodeFrameNoDelay(encoded->annexB.data(), static_cast<int>(encoded->annexB.size()), planes, &information);
        if (status != dsErrorFree || information.iBufferStatus != 1 ||
            information.UsrData.sSystemBuffer.iWidth != static_cast<int>(width) || information.UsrData.sSystemBuffer.iHeight != static_cast<int>(height)) {
            throw std::runtime_error("Benchmark frame failed immediate independent decoding");
        }
        if (frame >= warmup) {
            const auto decodedStride = information.UsrData.sSystemBuffer.iStride[0];
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto* pixel = pixels.data() + y * stride + x * 4;
                    const int expected = ((66 * pixel[2] + 129 * pixel[1] + 25 * pixel[0] + 128) >> 8) + 16;
                    const int error = planes[0][static_cast<std::size_t>(y) * decodedStride + x] - expected;
                    squaredError += error * error;
                }
            }
            elapsed.push_back(milliseconds);
            bytes += encoded->annexB.size();
            cpu += cpuElapsed;
        }
    }
    const auto total = std::accumulate(elapsed.begin(), elapsed.end(), 0.0);
    std::sort(elapsed.begin(), elapsed.end());
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles) ||
        !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        throw std::runtime_error("Cannot read benchmark resource usage");
    }
    std::cout << "{\"width\":" << width << ",\"height\":" << height << ",\"workload\":\"" << (scrolling ? "scroll" : "window")
              << "\",\"samples\":" << samples << ",\"mean_ms\":" << total / samples
              << ",\"p95_ms\":" << elapsed[samples * 95 / 100 - 1] << ",\"p99_ms\":" << elapsed[samples * 99 / 100 - 1]
              << ",\"encoder_fps\":" << samples * 1000 / total << ",\"cpu_ms_per_frame\":" << cpu / 10000.0 / samples
              << ",\"bytes_per_frame\":" << bytes / samples
              << ",\"process_private_bytes\":" << memory.PrivateUsage << ",\"process_handles\":" << handles
              << ",\"luma_psnr_db\":" << (squaredError ? 10 * std::log10(255.0 * 255 * samples * width * height / squaredError) : 100.0);
    if constexpr (requires { encoder.hardware(); }) {
        std::cout << ",\"hardware\":" << (encoder.hardware() ? "true" : "false");
    }
    std::cout << "}\n"
              << std::flush;
}
void measureConversion(std::uint32_t width, std::uint32_t height, ultraterimal::agent::VideoPixelFormat format) {
    constexpr unsigned warmup = 10, samples = 180;
    const auto stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(stride * height), output(static_cast<std::size_t>(width) * height * 3 / 2);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint8_t>((i * 17 + i / stride * 31) & 255);
    }
    std::vector<double> elapsed;
    elapsed.reserve(samples);
    std::uint64_t cpu = 0;
    for (unsigned frame = 0; frame < samples + warmup; ++frame) {
        const auto cpuStart = cpuTicks();
        const auto start = Clock::now();
        if (!ultraterimal::agent::convertVideoPixels(pixels, stride, width, height, format, output)) {
            throw std::runtime_error("Conversion benchmark rejected valid pixels");
        }
        const auto duration = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto cpuElapsed = cpuTicks() - cpuStart;
        if (frame >= warmup) {
            elapsed.push_back(duration);
            cpu += cpuElapsed;
        }
    }
    const auto total = std::accumulate(elapsed.begin(), elapsed.end(), 0.0);
    std::sort(elapsed.begin(), elapsed.end());
    std::cout << "{\"stage\":\"conversion\",\"width\":" << width << ",\"height\":" << height
              << ",\"format\":\"" << (format == ultraterimal::agent::VideoPixelFormat::kI420 ? "i420" : "nv12")
              << "\",\"samples\":" << samples << ",\"mean_ms\":" << total / samples
              << ",\"p95_ms\":" << elapsed[samples * 95 / 100 - 1] << ",\"p99_ms\":" << elapsed[samples * 99 / 100 - 1]
              << ",\"cpu_ms_per_frame\":" << cpu / 10000.0 / samples
              << ",\"checksum\":" << std::accumulate(output.begin(), output.end(), std::uint64_t{0}) << "}\n"
              << std::flush;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--capture-pipeline") {
            measureCapturePipeline();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--conversion") {
            for (const auto format : {ultraterimal::agent::VideoPixelFormat::kI420, ultraterimal::agent::VideoPixelFormat::kNv12}) {
                measureConversion(1280, 720, format);
                measureConversion(1920, 1080, format);
                measureConversion(2560, 1600, format);
            }
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--detail") {
            for (bool sharp : {false, true}) {
                std::cout << (sharp ? "Sharp\n" : "Balanced\n");
                measure(1920, 1080, true, sharp);
            }
            return 0;
        }
        if (argc != 1) {
            if (argc == 2 && std::string_view(argv[1]) == "--automatic") {
                for (bool scrolling : {false, true}) {
                    measure<ultraterimal::agent::VideoEncoder>(1280, 720, scrolling);
                    measure<ultraterimal::agent::VideoEncoder>(1920, 1080, scrolling);
                    measure<ultraterimal::agent::VideoEncoder>(2560, 1600, scrolling);
                }
                return 0;
            }
            throw std::invalid_argument("Usage: agent-software-video-benchmark [--conversion|--capture-pipeline|--detail]");
        }
        for (const bool scrolling : {false, true}) {
            measure(1280, 720, scrolling);
            measure(1920, 1080, scrolling);
            measure(2560, 1600, scrolling);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
