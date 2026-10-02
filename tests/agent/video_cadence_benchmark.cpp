#include <algorithm>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <ruvia/core/EventLoopAttachment.h>

#include "capture.fixture.h"
#include "features/desktop/video.runtime.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::uint64_t cpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("Cannot measure benchmark CPU time");
    }
    const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
asio::awaitable<void> measure(ultraterimal::agent::DesktopVideoRuntime& video, std::chrono::seconds duration, bool sharp, std::vector<Clock::time_point>& received, std::vector<double>& sourceAges, std::vector<std::uint64_t>& presentTicks, std::vector<std::uint64_t>& encodeStartedUs, std::int64_t qpcFrequency, unsigned& sourceWidth, unsigned& sourceHeight, unsigned& lastTargetRate,
                              unsigned& hardwareFrames, unsigned& gpuFrames, std::uint64_t& totalEncodeMicroseconds, std::uint64_t& totalEncodedBytes, unsigned& calls, unsigned& noUpdate, unsigned& cursorOnly,
                              ultraterimal::agent::DesktopVideoDiagnostics& captured) {
    using namespace ultraterimal;
    std::optional<agent::DesktopVideoUpdate> first;
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline && (!first || first->frame.data().empty())) {
        first = co_await video.next();
    }
    if (!first || first->frame.data().empty() || !first->configuration) {
        throw std::runtime_error("Cannot establish desktop video baseline and geometry");
    }
    sourceWidth = first->configuration->width();
    sourceHeight = first->configuration->height();
    wire::QualityFeedback feedback;
    feedback.set_preference(sharp ? wire::QualityFeedback::SHARP : wire::QualityFeedback::SMOOTH);
    feedback.set_displayed_generation(first->generation);
    feedback.set_displayed_frame(first->frameId);
    video.feedback(feedback);
    const auto until = Clock::now() + 3s + duration;
    const auto warmup = until - duration;
    agent::DesktopVideoDiagnostics baseline;
    bool sampling = false;
    while (Clock::now() < until) {
        const auto before = video.diagnostics();
        auto update = co_await video.next();
        const auto now = Clock::now();
        if (now >= warmup && !sampling) {
            baseline = before;
            sampling = true;
        }
        if (now >= warmup && now < until) {
            ++calls;
            if (!update) {
                ++noUpdate;
            } else if (update->frame.data().empty()) {
                ++cursorOnly;
            }
        }
        if (!update || update->frame.data().empty()) {
            continue;
        }
        feedback.set_displayed_generation(update->generation);
        feedback.set_displayed_frame(update->frameId);
        feedback.set_decode_microseconds(1000);
        feedback.set_queue_depth(0);
        video.feedback(feedback);
        if (now >= warmup && now < until) {
            received.push_back(now);
            if (update->sourcePresentTicks) {
                LARGE_INTEGER published{};
                if (!QueryPerformanceCounter(&published) || static_cast<std::uint64_t>(published.QuadPart) < update->sourcePresentTicks) {
                    throw std::runtime_error("Invalid DXGI present timestamp in video runtime");
                }
                sourceAges.push_back(1000.0 * (static_cast<std::uint64_t>(published.QuadPart) - update->sourcePresentTicks) / qpcFrequency);
                presentTicks.push_back(update->sourcePresentTicks);
                encodeStartedUs.push_back(update->frame.timestamp_microseconds());
            }
            lastTargetRate = update->frame.target_frames_per_second();
            hardwareFrames += update->frame.hardware_encoding();
            gpuFrames += update->frame.gpu_encoding();
            totalEncodeMicroseconds += update->frame.encode_microseconds();
            totalEncodedBytes += update->frame.data().size();
        }
    }
    const auto after = video.diagnostics();
    captured = {after.captureCalls - baseline.captureCalls,
                after.pixelUpdates - baseline.pixelUpdates,
                after.cursorOnlyUpdates - baseline.cursorOnlyUpdates,
                after.captureTimeouts - baseline.captureTimeouts,
                after.pendingBeforeDeadlineCalls - baseline.pendingBeforeDeadlineCalls,
                after.backpressuredCalls - baseline.backpressuredCalls,
                after.encodedFrames - baseline.encodedFrames,
                after.pixelCaptureGaps - baseline.pixelCaptureGaps,
                after.pixelCaptureGapsGe45ms - baseline.pixelCaptureGapsGe45ms};
    video.stop();
}
} // namespace
int main(int argc, char** argv) {
    try {
        unsigned paintHz = 40;
        int paintSize = 64;
        auto duration = 10s;
        bool sharp = false;
        bool scrolling = false;
        bool motion = false;
        for (int index = 1; index < argc; ++index) {
            const auto option = std::string_view(argv[index]);
            if (option == "--paint-hz=30") {
                paintHz = 30;
            } else if (option == "--paint-hz=40") {
                paintHz = 40;
            } else if (option == "--paint-hz=60") {
                paintHz = 60;
            } else if (option == "--seconds=60") {
                duration = 60s;
            } else if (option == "--paint-size=512") {
                paintSize = 512;
            } else if (option == "--paint-size=960") {
                paintSize = 960;
            } else if (option == "--quality=sharp") {
                sharp = true;
            } else if (option == "--scroll") {
                scrolling = true;
            } else if (option == "--motion") {
                motion = true;
            } else {
                throw std::invalid_argument("Usage: agent-video-cadence-benchmark [--paint-hz=30|40|60] [--paint-size=512|960] [--seconds=60] [--quality=sharp] [--scroll|--motion]");
            }
        }
        if (scrolling && motion) {
            throw std::invalid_argument("Scroll and motion are mutually exclusive");
        }
        const auto paintInterval = std::chrono::microseconds(1000000 / paintHz);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        unsigned paints = 0;
        std::exception_ptr painterFailure;
        std::jthread painter([&](std::stop_token stop) {
            try {
                ultraterimal::test::TestSurface surface(paintSize);
                const auto began = Clock::now();
                while (!stop.stop_requested() && paints < paintHz * (duration.count() + 5)) {
                    std::this_thread::sleep_until(began + paintInterval * paints);
                    if (stop.stop_requested()) {
                        break;
                    }
                    const auto color = paints % 2 ? RGB(208, 32, 32) : RGB(32, 208, 32);
                    if (scrolling) {
                        surface.scroll(color);
                    } else if (motion) {
                        surface.paintMotion(paints);
                    } else {
                        surface.paint(color);
                    }
                    ++paints;
                }
            } catch (...) {
                painterFailure = std::current_exception();
            }
        });
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        ultraterimal::agent::DesktopVideoRuntime video(attachment.loop(), 0);
        std::vector<Clock::time_point> received;
        std::vector<double> sourceAges;
        std::vector<std::uint64_t> presentTicks;
        std::vector<std::uint64_t> encodeStartedUs;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            throw std::runtime_error("Cannot measure DXGI present age");
        }
        unsigned sourceWidth = 0, sourceHeight = 0, targetRate = 0, hardwareFrames = 0, gpuFrames = 0, calls = 0, noUpdate = 0, cursorOnly = 0;
        std::uint64_t totalEncodeMicroseconds = 0, totalEncodedBytes = 0;
        ultraterimal::agent::DesktopVideoDiagnostics captured;
        std::exception_ptr failure;
        asio::co_spawn(io, measure(video, duration, sharp, received, sourceAges, presentTicks, encodeStartedUs, frequency.QuadPart, sourceWidth, sourceHeight, targetRate, hardwareFrames, gpuFrames, totalEncodeMicroseconds, totalEncodedBytes, calls, noUpdate, cursorOnly, captured), [&](std::exception_ptr error) {
            failure = error;
            video.stop();
            attachment.stop();
        });
        const auto cpuStarted = cpuTicks();
        attachment.run();
        painter.request_stop();
        painter.join();
        const auto cpuMilliseconds = (cpuTicks() - cpuStarted) / 10000.0;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            throw std::runtime_error("Cannot measure benchmark process memory");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (painterFailure) {
            std::rethrow_exception(painterFailure);
        }
        if (received.size() < 2 || presentTicks.size() != received.size() || encodeStartedUs.size() != received.size()) {
            throw std::runtime_error("No sustained video cadence or DXGI present timestamps were recorded");
        }
        std::vector<double> gaps;
        std::vector<double> sourceGaps;
        std::vector<double> encodeStartGaps;
        unsigned longOutputGaps = 0, longOutputGapsGe66ms = 0, longOutputGapsGe100ms = 0, longSourceGaps = 0, longOutputWithShortSource = 0, longOutputWithShortEncodeStart = 0;
        for (std::size_t index = 1; index < received.size(); ++index) {
            if (presentTicks[index] <= presentTicks[index - 1] || encodeStartedUs[index] <= encodeStartedUs[index - 1]) {
                throw std::runtime_error("DXGI presentation or encoding timestamps did not advance with each frame");
            }
            const auto encodeStartGap = (encodeStartedUs[index] - encodeStartedUs[index - 1]) / 1000.0;
            encodeStartGaps.push_back(encodeStartGap);
            const auto gap = std::chrono::duration<double, std::milli>(received[index] - received[index - 1]).count();
            const auto sourceGap = 1000.0 * (presentTicks[index] - presentTicks[index - 1]) / frequency.QuadPart;
            gaps.push_back(gap);
            sourceGaps.push_back(sourceGap);
            longOutputGaps += gap >= 45;
            longOutputGapsGe66ms += gap >= 66;
            longOutputGapsGe100ms += gap >= 100;
            longSourceGaps += sourceGap >= 45;
            longOutputWithShortSource += gap >= 45 && sourceGap < 35;
            longOutputWithShortEncodeStart += gap >= 45 && encodeStartGap < 35;
        }
        const auto elapsed = std::chrono::duration<double>(received.back() - received.front()).count();
        std::sort(gaps.begin(), gaps.end());
        std::sort(sourceGaps.begin(), sourceGaps.end());
        std::sort(encodeStartGaps.begin(), encodeStartGaps.end());
        std::sort(sourceAges.begin(), sourceAges.end());
        const auto action = scrolling ? "scroll" : (motion ? "motion" : "paint");
        std::cout << "{\"workload\":\"real_dxgi_paint_to_video_runtime\",\"paint_hz\":" << paintHz << ",\"quality\":\"" << (sharp ? "sharp" : "smooth")
                  << "\",\"surface_action\":\"" << action << "\",\"paint_size\":" << paintSize << ",\"source_width\":" << sourceWidth << ",\"source_height\":" << sourceHeight
                  << ",\"duration_seconds\":" << duration.count() << ",\"paint_calls\":" << paints
                  << ",\"target_fps\":" << targetRate << ",\"encoded_frames\":" << received.size() << ",\"measured_seconds\":" << elapsed
                  << ",\"hardware_frames\":" << hardwareFrames << ",\"gpu_frames\":" << gpuFrames << ",\"mean_encode_us\":" << totalEncodeMicroseconds / received.size() << ",\"mean_encoded_bytes\":" << totalEncodedBytes / received.size() << ",\"encoded_bps\":" << totalEncodedBytes * 8 / elapsed
                  << ",\"video_next_calls\":" << calls << ",\"no_update_calls\":" << noUpdate << ",\"cursor_only_calls\":" << cursorOnly
                  << ",\"dxgi_calls\":" << captured.captureCalls << ",\"pixel_updates\":" << captured.pixelUpdates << ",\"dxgi_timeouts\":" << captured.captureTimeouts
                  << ",\"pending_before_deadline_calls\":" << captured.pendingBeforeDeadlineCalls << ",\"backpressured_calls\":" << captured.backpressuredCalls
                  << ",\"worker_encoded_frames\":" << captured.encodedFrames
                  << ",\"pixel_capture_gaps\":" << captured.pixelCaptureGaps << ",\"pixel_capture_gaps_ge_45ms\":" << captured.pixelCaptureGapsGe45ms
                  << ",\"cpu_ms_including_warmup\":" << cpuMilliseconds << ",\"peak_working_set_kib\":" << memory.PeakWorkingSetSize / 1024
                  << ",\"private_bytes_kib\":" << memory.PrivateUsage / 1024
                  << ",\"effective_fps\":" << (received.size() - 1) / elapsed << ",\"gap_p05_ms\":" << gaps[(gaps.size() * 5 + 99) / 100 - 1]
                  << ",\"gap_p50_ms\":" << gaps[gaps.size() / 2] << ",\"gap_p95_ms\":" << gaps[(gaps.size() * 95 + 99) / 100 - 1]
                  << ",\"gap_p99_ms\":" << gaps[(gaps.size() * 99 + 99) / 100 - 1]
                  << ",\"source_gap_p95_ms\":" << sourceGaps[(sourceGaps.size() * 95 + 99) / 100 - 1]
                  << ",\"source_gap_p99_ms\":" << sourceGaps[(sourceGaps.size() * 99 + 99) / 100 - 1]
                  << ",\"encode_start_gap_p95_ms\":" << encodeStartGaps[(encodeStartGaps.size() * 95 + 99) / 100 - 1]
                  << ",\"encode_start_gap_p99_ms\":" << encodeStartGaps[(encodeStartGaps.size() * 99 + 99) / 100 - 1]
                  << ",\"output_gaps_ge_45ms\":" << longOutputGaps << ",\"output_gaps_ge_66ms\":" << longOutputGapsGe66ms
                  << ",\"output_gaps_ge_100ms\":" << longOutputGapsGe100ms << ",\"source_gaps_ge_45ms\":" << longSourceGaps
                  << ",\"output_gaps_ge_45ms_with_source_lt_35ms\":" << longOutputWithShortSource
                  << ",\"output_gaps_ge_45ms_with_encode_start_lt_35ms\":" << longOutputWithShortEncodeStart
                  << ",\"source_age_p50_ms\":" << sourceAges[sourceAges.size() / 2]
                  << ",\"source_age_p95_ms\":" << sourceAges[(sourceAges.size() * 95 + 99) / 100 - 1] << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
