#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_attachment.h>

#include "config/command_line.config.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/input.transport.h"
#include "platform/windows/pipe.transport.h"

namespace {
using namespace ultraterimal::agent;
asio::awaitable<void> child(ruvia::event_loop loop, const CommandLine& options) {
    DesktopParent parent(options.windowsSession, options.parentProcess);
    auto pipe = NamedPipeTransport::connect(loop.executor(), options.pipeName, options.parentProcess);
    std::string result;
    std::string_view stage = "create desktop capture";
    try {
        DesktopCapture capture;
        stage = "read desktop frame";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::optional<CapturedFrame> frame;
        while (!frame && std::chrono::steady_clock::now() < deadline) {
            frame = capture.nextFrame(std::chrono::milliseconds(100));
        }
        if (!frame) {
            throw std::runtime_error("No captured frame for input geometry");
        }
        stage = "initialize desktop input";
        DesktopInput input(frame->desktopLeft, frame->desktopTop, frame->width, frame->height);
        input.releaseAll();
        result = "PASS: desktop child capture geometry and input initialization";
    } catch (const std::exception& error) {
        result = std::string(stage) + ": " + error.what();
    }
    co_await pipe->send(std::move(result));
    (void)co_await pipe->receive();
}
asio::awaitable<void> parent(ruvia::event_loop loop, std::filesystem::path executable) {
    DesktopIdentity identity(currentDesktopSession());
    NamedPipeTransport pipe(loop.executor(), identity.pipeSid());
    DesktopProcess process(loop.executor(), identity, executable, pipe.name());
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(std::chrono::seconds(10));
    deadline.async_wait([&](std::error_code error) {
        if (!error) {
            pipe.cancel();
            process.terminate();
        }
    });
    try {
        co_await pipe.accept(process.id(), identity.session());
        const auto result = co_await pipe.receive();
        co_await pipe.send("ack");
        const auto code = co_await process.wait();
        if (code != 0 || !result.starts_with("PASS:")) {
            throw std::runtime_error(result);
        }
        std::cout << result << '\n';
        deadline.cancel();
    } catch (...) {
        deadline.cancel();
        throw;
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io);
        std::vector<std::wstring_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        const auto options = parseCommandLine(arguments);
        if (!options || (argc > 1 && options->command != Command::kDesktop)) {
            return 2;
        }
        std::exception_ptr failure;
        asio::co_spawn(attachment.loop().executor(), argc == 1 ? parent(attachment.loop(), std::filesystem::absolute(argv[0])) : child(attachment.loop(), *options), [&](std::exception_ptr error) {
            failure = error;
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
