#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <windows.h>

#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/event_loop.h>

#include "features/desktop/video.runtime.h"

namespace {
std::uint64_t cpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("Cannot read process CPU time");
    }
    const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
asio::awaitable<void> measure(ultraterimal::agent::DesktopVideoRuntime& video) {
    using Clock = std::chrono::steady_clock;
    const auto warmup = Clock::now() + std::chrono::seconds(2);
    const auto consume = [&](const ultraterimal::agent::DesktopVideoUpdate& update) {
        if (!update.frame.data().empty()) {
            ultraterimal::wire::QualityFeedback feedback;
            feedback.set_displayed_generation(update.generation);
            feedback.set_displayed_frame(update.frameId);
            feedback.set_background(true);
            video.feedback(feedback);
        }
    };
    while (Clock::now() < warmup) {
        if (const auto update = co_await video.next()) {
            consume(*update);
        }
    }
    unsigned calls = 0, frames = 0;
    const auto started = Clock::now();
    const auto cpu = cpuTicks();
    while (Clock::now() - started < std::chrono::seconds(10)) {
        const auto update = co_await video.next();
        ++calls;
        if (update) {
            consume(*update);
            frames += !update->frame.data().empty();
        }
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    std::cout << "{\"workload\":\"hidden_desktop\",\"seconds\":" << elapsed << ",\"calls\":" << calls << ",\"frames\":" << frames
              << ",\"cpu_ms\":" << (cpuTicks() - cpu) / 10000.0 << "}\n";
}
} // namespace
int main() {
    try {
        asio::io_context io;
        ruvia::blocking_pool blockingPool{{.thread_count_ = 1, .queue_capacity_ = 4}};
        auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blockingPool});
        ultraterimal::agent::DesktopVideoRuntime video(attachment.loop(), blockingPool, 0);
        std::exception_ptr failure;
        asio::co_spawn(attachment.loop().executor(), measure(video), [&](std::exception_ptr error) {
            failure = error;
            video.stop();
            attachment.stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
