#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
void measureCapturePipeline(bool verifyReadback, bool forceStaging) {
    using namespace ultraterimal;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    test::TestSurface surface;
    agent::DesktopCapture capture;
    std::unique_ptr<agent::SoftwareVideoEncoder> encoder;
    std::vector<std::uint8_t> pixels;
    std::vector<double> painting, capturing, encoding, paintToPresent, presentToCapture, acquire, copyDispatch, mapWait, rowCopy;
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        throw std::runtime_error("Cannot measure DXGI present timestamps");
    }
    constexpr unsigned warmup = 10, samples = 180;
    std::uint32_t width = 0, height = 0;
    std::uint64_t bytes = 0, copiedPixels = 0;
    unsigned captureCalls = 0, pixelFrames = 0, presentedFrames = 0, verifiedReadbacks = 0;
    const auto cpu = cpuTicks();
    for (unsigned index = 0; index < warmup + samples; ++index) {
        const auto color = index % 2 ? RGB(208, 32, 32) : RGB(32, 208, 32);
        const auto started = Clock::now();
        LARGE_INTEGER paintStartedTicks{};
        QueryPerformanceCounter(&paintStartedTicks);
        surface.paint(color);
        const auto painted = Clock::now();
        std::optional<agent::CapturedFrame> frame;
        agent::CaptureTiming captureTiming;
        LARGE_INTEGER returnedTicks{};
        const auto deadline = painted + std::chrono::seconds(2);
        while (Clock::now() < deadline) {
            agent::CaptureTiming timing;
            timing.forceStaging = forceStaging;
            timing.verifyWithStaging = verifyReadback;
            auto candidate = capture.nextFrame(std::chrono::milliseconds(8), &pixels, &timing);
            if (candidate && !candidate->bgra.empty() && verifyReadback) {
                if (!timing.matchedStaging) {
                    throw std::runtime_error("Controlled DXGI frame could not be compared with full staging readback");
                }
                ++verifiedReadbacks;
            }
            QueryPerformanceCounter(&returnedTicks);
            ++captureCalls;
            if (candidate && candidate->captureTicks) {
                ++presentedFrames;
            }
            if (!candidate || candidate->bgra.empty()) {
                continue;
            }
            ++pixelFrames;
            if (candidate->width <= 64 || candidate->height <= 64) {
                throw std::runtime_error("Capture benchmark display is too small");
            }
            const auto offset = (static_cast<std::size_t>(64) * candidate->width + 64) * 4;
            if (candidate->bgra[offset] == GetBValue(color) && candidate->bgra[offset + 1] == GetGValue(color) && candidate->bgra[offset + 2] == GetRValue(color)) {
                frame = std::move(candidate);
                captureTiming = timing;
                break;
            }
            pixels = std::move(candidate->bgra);
        }
        if (!frame) {
            throw std::runtime_error("Capture benchmark did not observe its new test pixels; capture_calls=" + std::to_string(captureCalls) +
                                     " pixel_frames=" + std::to_string(pixelFrames) + " presented_frames=" + std::to_string(presentedFrames) +
                                     " test_window_visible=" + (surface.visible() ? "true" : "false") + " test_window_on_top=" + (surface.centerOnTop() ? "true" : "false"));
        }
        if (!frame->captureTicks || static_cast<std::uint64_t>(returnedTicks.QuadPart) < frame->captureTicks ||
            static_cast<std::uint64_t>(paintStartedTicks.QuadPart) > frame->captureTicks) {
            throw std::runtime_error("DXGI present timestamp is outside the paint/capture interval");
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
            const auto ticksToMilliseconds = 1000.0 / static_cast<double>(frequency.QuadPart);
            // DXGI LastPresentTime uses QPC ticks. This interval includes the
            // test's GDI paint, while the interval after present includes
            // scheduling, AcquireNextFrame and the complete GPU readback.
            paintToPresent.push_back((static_cast<double>(frame->captureTicks) - static_cast<double>(paintStartedTicks.QuadPart)) * ticksToMilliseconds);
            presentToCapture.push_back((static_cast<double>(returnedTicks.QuadPart) - static_cast<double>(frame->captureTicks)) * ticksToMilliseconds);
            encoding.push_back(std::chrono::duration<double, std::milli>(encodedAt - encodeStarted).count());
            acquire.push_back(captureTiming.acquireMilliseconds);
            copyDispatch.push_back(captureTiming.copyDispatchMilliseconds);
            mapWait.push_back(captureTiming.mapMilliseconds);
            rowCopy.push_back(captureTiming.rowCopyMilliseconds);
            copiedPixels += static_cast<std::uint64_t>(width) * height;
            bytes += encoded->annexB.size();
        }
    }
    const auto cpuMilliseconds = (cpuTicks() - cpu) / 10000.0;
    const std::string_view readback = forceStaging ? "full_staging" : (verifyReadback ? "full_verified" : "full");
    const auto report = [&](std::string_view stage, std::vector<double> values) {
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        std::sort(values.begin(), values.end());
        std::cout << "{\"workload\":\"real_desktop_64px_patch\",\"readback\":\"" << readback
                  << "\",\"stage\":\"" << stage << "\",\"width\":" << width
                  << ",\"height\":" << height << ",\"samples\":" << samples << ",\"cpu_ms\":" << cpuMilliseconds
                  << ",\"verified_readbacks_including_warmup\":" << verifiedReadbacks
                  << ",\"copied_pixels\":" << copiedPixels << ",\"mean_ms\":" << mean
                  << ",\"p95_ms\":" << values[(samples * 95 + 99) / 100 - 1] << ",\"p99_ms\":" << values[(samples * 99 + 99) / 100 - 1]
                  << ",\"verified_frames_including_warmup\":" << warmup + samples << ",\"capture_calls_including_warmup\":" << captureCalls
                  << ",\"encoded_bytes_per_frame\":" << bytes / samples << "}\n";
    };
    report("test_paint_and_compositor_wait", std::move(painting));
    report("capture_until_verified_pixels", std::move(capturing));
    report("acquire", std::move(acquire));
    report("copy_dispatch", std::move(copyDispatch));
    report("map_wait", std::move(mapWait));
    report("row_copy_and_unmap", std::move(rowCopy));
    report("paint_start_to_dxgi_present", std::move(paintToPresent));
    report("dxgi_present_to_capture_return", std::move(presentToCapture));
    report("conversion_and_encode", std::move(encoding));
}
void measureCaptureReadback(bool forceStaging, bool verifyWithStaging) {
    using namespace ultraterimal;
    constexpr std::size_t samples = 20;
    std::array<std::vector<double>, 4> stages;
    std::uint32_t width = 0, height = 0;
    bool desktopImageInSystemMemory = false;
    std::size_t verifiedFrames = 0;
    for (std::size_t index = 0; index < samples; ++index) {
        agent::DesktopCapture capture;
        agent::CaptureTiming timing;
        timing.forceStaging = forceStaging;
        timing.verifyWithStaging = verifyWithStaging;
        const auto frame = capture.nextFrame(std::chrono::milliseconds(1000), nullptr, &timing);
        if (!frame || frame->bgra.empty()) {
            throw std::runtime_error("DXGI duplication did not provide an initial pixel snapshot");
        }
        if (verifyWithStaging && !timing.matchedStaging) {
            throw std::runtime_error("DXGI output does not support verified system-memory readback");
        }
        width = frame->width;
        height = frame->height;
        desktopImageInSystemMemory = timing.desktopImageInSystemMemory;
        verifiedFrames += timing.matchedStaging;
        stages[0].push_back(timing.acquireMilliseconds);
        stages[1].push_back(timing.copyDispatchMilliseconds);
        stages[2].push_back(timing.mapMilliseconds);
        stages[3].push_back(timing.rowCopyMilliseconds);
    }
    constexpr std::array<std::string_view, 4> names = {"acquire", "copy_dispatch", "map_wait", "row_copy_and_unmap"};
    for (std::size_t index = 0; index < stages.size(); ++index) {
        auto& values = stages[index];
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / samples;
        std::sort(values.begin(), values.end());
        std::cout << "{\"workload\":\"initial_dxgi_readback\",\"path\":\"" << (forceStaging ? "staging" : "automatic") << "\",\"stage\":\"" << names[index] << "\",\"width\":" << width << ",\"height\":" << height
                  << ",\"system_memory\":" << (desktopImageInSystemMemory ? "true" : "false") << ",\"verified_frames\":" << verifiedFrames << ",\"samples\":" << samples << ",\"mean_ms\":" << mean << ",\"p95_ms\":" << values[(samples * 95 + 99) / 100 - 1]
                  << ",\"p99_ms\":" << values[(samples * 99 + 99) / 100 - 1] << "}\n";
    }
}
void verifyLiveCaptureReadback() {
    using namespace ultraterimal;
    agent::DesktopCapture capture;
    std::vector<std::uint8_t> pixels;
    std::vector<double> mapWait, rowCopy, stagingMapWait, stagingRowCopy, acquire;
    unsigned timeouts = 0, cursorOnly = 0, verified = 0;
    std::uint64_t copiedPixels = 0;
    const auto started = Clock::now();
    const auto cpu = cpuTicks();
    while (Clock::now() - started < std::chrono::seconds(30)) {
        agent::CaptureTiming timing;
        timing.verifyWithStaging = true;
        auto frame = capture.nextFrame(std::chrono::milliseconds(8), &pixels, &timing);
        if (!frame) {
            ++timeouts;
            continue;
        }
        if (frame->bgra.empty()) {
            ++cursorOnly;
            continue;
        }
        if (!timing.matchedStaging) {
            throw std::runtime_error("Live DXGI readback could not verify against staging");
        }
        ++verified;
        copiedPixels += static_cast<std::uint64_t>(frame->width) * frame->height;
        acquire.push_back(timing.acquireMilliseconds);
        mapWait.push_back(timing.mapMilliseconds);
        rowCopy.push_back(timing.rowCopyMilliseconds);
        stagingMapWait.push_back(timing.verificationStagingMapMilliseconds);
        stagingRowCopy.push_back(timing.verificationStagingRowCopyMilliseconds);
        pixels = std::move(frame->bgra);
    }
    if (!verified) {
        throw std::runtime_error("No changing desktop pixels were captured for live readback verification");
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    const auto cpuMilliseconds = (cpuTicks() - cpu) / 10000.0;
    const auto report = [&](std::string_view stage, std::vector<double> values) {
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        std::sort(values.begin(), values.end());
        const auto count = values.size();
        std::cout << "{\"workload\":\"live_dxgi_readback_verify\",\"stage\":\"" << stage << "\",\"seconds\":" << elapsed
                  << ",\"verified_frames\":" << verified
                  << ",\"copied_pixels\":" << copiedPixels
                  << ",\"cursor_only\":" << cursorOnly << ",\"timeouts\":" << timeouts << ",\"cpu_ms\":" << cpuMilliseconds << ",\"mean_ms\":" << mean
                  << ",\"p95_ms\":" << values[(count * 95 + 99) / 100 - 1] << ",\"p99_ms\":" << values[(count * 99 + 99) / 100 - 1] << "}\n";
    };
    report("acquire", std::move(acquire));
    report("map_wait", std::move(mapWait));
    report("row_copy_and_unmap", std::move(rowCopy));
    report("verification_staging_map_wait", std::move(stagingMapWait));
    report("verification_staging_row_copy_and_unmap", std::move(stagingRowCopy));
}
void verifyLiveScrollCapture(bool forceStaging) {
    using namespace ultraterimal;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    test::TestSurface surface;
    surface.paint(RGB(208, 32, 32));
    agent::DesktopCapture capture;
    std::vector<std::uint8_t> pixels;
    unsigned frames = 0;
    constexpr unsigned scrolls = 180;
    for (unsigned index = 0; index < scrolls; ++index) {
        const auto strip = index % 2 ? RGB(208, 32, 32) : RGB(32, 208, 32);
        surface.scroll(strip);
        const auto point = surface.stripPosition();
        bool matched = false;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < deadline) {
            agent::CaptureTiming timing;
            timing.forceStaging = forceStaging;
            timing.verifyWithStaging = true;
            auto frame = capture.nextFrame(std::chrono::milliseconds(8), &pixels, &timing);
            if (!frame || frame->bgra.empty()) {
                continue;
            }
            if (!timing.matchedStaging) {
                throw std::runtime_error("Scrolled desktop differed from full staging readback");
            }
            ++frames;
            const auto x = static_cast<std::int64_t>(point.x) - frame->desktopLeft;
            const auto y = static_cast<std::int64_t>(point.y) - frame->desktopTop;
            if (x < 0 || y < 0 || x >= frame->width || y >= frame->height) {
                throw std::runtime_error("Scrolled test surface is outside the captured display");
            }
            const auto pixel = (static_cast<std::size_t>(y) * frame->width + static_cast<std::size_t>(x)) * 4;
            matched = frame->bgra[pixel] == GetBValue(strip) && frame->bgra[pixel + 1] == GetGValue(strip) && frame->bgra[pixel + 2] == GetRValue(strip);
            pixels = std::move(frame->bgra);
            if (matched) {
                break;
            }
        }
        if (!matched) {
            throw std::runtime_error("DXGI did not show scrolled test pixels; test_window_visible=" + std::string(surface.visible() ? "true" : "false") +
                                     " test_window_on_top=" + (surface.centerOnTop() ? "true" : "false"));
        }
    }
    std::cout << "{\"workload\":\"real_dxgi_scroll_verify\",\"path\":\"" << (forceStaging ? "staging" : "automatic")
              << "\",\"scrolls\":" << scrolls << ",\"verified_frames\":" << frames
              << "}\n";
}
template <typename Encoder = ultraterimal::agent::SoftwareVideoEncoder>
void measure(std::uint32_t width, std::uint32_t height, bool scrolling, bool sharp = false, std::uint32_t bitrate = 8000000, bool motionStress = false, std::uint32_t frameRate = 60) {
    constexpr unsigned warmup = 10, samples = 180;
    const auto stride = static_cast<std::size_t>(width) * 4;
    std::vector<std::uint8_t> pixels(stride * height);
    Encoder encoder({width, height, bitrate, frameRate, sharp});
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
                auto* pixel = pixels.data() + y * stride + x * 4;
                if (motionStress) {
                    // Deterministic 8x8 moving texture: intentionally less
                    // compressible than scrolling text, not actual video.
                    std::uint32_t pattern = (x / 8 * 73856093u) ^ (y / 8 * 19349663u) ^ (frame * 83492791u);
                    pattern ^= pattern >> 13;
                    pattern *= 1274126177u;
                    pattern ^= pattern >> 16;
                    const auto shade = static_cast<std::uint8_t>(32 + pattern % 192);
                    pixel[0] = pixel[1] = pixel[2] = shade;
                } else {
                    const auto row = y + (scrolling ? frame * 7 : 0);
                    const bool text = row % 24 < 3 && x % 180 < 130;
                    const bool window = x >= left && x < left + 320 && y >= 100 && y < 340;
                    pixel[0] = window ? 180 : text ? 30
                                                   : 240;
                    pixel[1] = window ? 90 : text ? 30
                                                  : 240;
                    pixel[2] = window ? 40 : text ? 30
                                                  : 240;
                }
                pixel[3] = 255;
            }
        }
        const auto cpuStart = cpuTicks();
        const auto started = Clock::now();
        const auto encoded = encoder.encode(pixels, stride, frame * (1000000ull / frameRate));
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
    const auto workload = motionStress ? "motion_stress" : (scrolling ? "scroll" : "window");
    std::cout << "{\"width\":" << width << ",\"height\":" << height << ",\"bitrate\":" << bitrate << ",\"nominal_fps\":" << frameRate << ",\"sharp\":" << (sharp ? "true" : "false") << ",\"workload\":\"" << workload
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
void measureConversion(std::uint32_t width, std::uint32_t height, ultraterimal::agent::VideoPixelFormat format, ultraterimal::agent::VideoPixelBackend backend) {
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
        if (!ultraterimal::agent::convertVideoPixels(pixels, stride, width, height, format, output, backend)) {
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
    const auto avx2 = backend != ultraterimal::agent::VideoPixelBackend::kSse2 && ultraterimal::agent::videoPixelAvx2Available();
    std::cout << "{\"stage\":\"conversion\",\"backend\":\"" << (avx2 ? "avx2" : "sse2") << "\",\"width\":" << width << ",\"height\":" << height
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
        if (argc == 2 && (std::string_view(argv[1]) == "--capture-pipeline" || std::string_view(argv[1]) == "--capture-pipeline-verify" || std::string_view(argv[1]) == "--capture-pipeline-staging")) {
            measureCapturePipeline(std::string_view(argv[1]) == "--capture-pipeline-verify", std::string_view(argv[1]) == "--capture-pipeline-staging");
            return 0;
        }
        if (argc == 2 && (std::string_view(argv[1]) == "--capture-readback" || std::string_view(argv[1]) == "--capture-readback-staging" || std::string_view(argv[1]) == "--capture-readback-verify")) {
            measureCaptureReadback(std::string_view(argv[1]) == "--capture-readback-staging", std::string_view(argv[1]) == "--capture-readback-verify");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--capture-readback-live-verify") {
            verifyLiveCaptureReadback();
            return 0;
        }
        if (argc == 2 && (std::string_view(argv[1]) == "--capture-scroll-verify" || std::string_view(argv[1]) == "--capture-scroll-staging-verify")) {
            verifyLiveScrollCapture(std::string_view(argv[1]) == "--capture-scroll-staging-verify");
            return 0;
        }
        if (argc == 2 && (std::string_view(argv[1]) == "--conversion" || std::string_view(argv[1]) == "--conversion-sse2" || std::string_view(argv[1]) == "--conversion-avx2")) {
            using ultraterimal::agent::VideoPixelBackend;
            const auto backend = std::string_view(argv[1]) == "--conversion-sse2" ? VideoPixelBackend::kSse2 : std::string_view(argv[1]) == "--conversion-avx2" ? VideoPixelBackend::kAvx2
                                                                                                                                                                : VideoPixelBackend::kAutomatic;
            if (backend == VideoPixelBackend::kAvx2 && !ultraterimal::agent::videoPixelAvx2Available()) {
                throw std::runtime_error("AVX2 conversion requires CPU and OS AVX2 support");
            }
            for (const auto format : {ultraterimal::agent::VideoPixelFormat::kI420, ultraterimal::agent::VideoPixelFormat::kNv12}) {
                measureConversion(1280, 720, format, backend);
                measureConversion(1920, 1080, format, backend);
                measureConversion(2560, 1600, format, backend);
            }
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--motion-stress") {
            measure(1920, 1080, false, false, 8000000, true, 30);
            measure(1920, 1080, false, true, 8000000, true, 30);
            measure(1920, 1080, false, true, 24000000, true, 30);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--detail-live-rate") {
            for (bool scrolling : {false, true}) {
                measure(1920, 1080, scrolling, true, 24000000);
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
            throw std::invalid_argument("Usage: agent-software-video-benchmark [--conversion|--conversion-sse2|--conversion-avx2|--capture-pipeline|--capture-pipeline-verify|--capture-pipeline-staging|--capture-readback|--capture-readback-staging|--capture-readback-verify|--capture-readback-live-verify|--capture-scroll-verify|--capture-scroll-staging-verify|--detail|--detail-live-rate|--motion-stress|--automatic]");
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
