#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/windows/object_handle.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <ruvia/core/async.h>
#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/event_loop.h>

#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/pipe.transport.h"

namespace {
using namespace ultraterimal;
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
template <class F>
void reject(F&& action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "Invalid launch accepted");
}
asio::awaitable<void> exercise(ruvia::event_loop loop, std::filesystem::path executable) {
    DWORD session = 0;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session, "Interactive session required");
    reject([] { agent::DesktopIdentity invalid(0); });
    reject([] { agent::DesktopIdentity invalid(0xffffffff); });
    reject([&] { agent::DesktopIdentity invalid(session + 1); });
    agent::DesktopIdentity user(session);
    require(user.session() == session && user.pipeSid() == agent::currentLogonSid(), "Incorrect desktop user");
    for (const auto sid : {L"S-1-5-18", L"S-1-5-20", L"S-1-1-0", L"S-1-5-5-1-2)(A;;GA;;;WD)"}) {
        reject([&] { agent::NamedPipeTransport invalid(loop.executor(), sid); });
    }
    agent::NamedPipeTransport pipe(loop.executor(), user.pipeSid());
    reject([&] { agent::DesktopProcess invalid(loop.executor(), user, L"agent.exe", pipe.name()); });
    reject([&] { agent::DesktopProcess invalid(loop.executor(), user, executable, pipe.name() + L" --service"); });
    reject([&] { agent::DesktopProcess invalid(loop.executor(), user, executable.parent_path() / L"nonexistent-launch-test.exe", pipe.name()); });
    auto child = std::make_unique<agent::DesktopProcess>(loop.executor(), user, executable, pipe.name());
    require(child->id() && child->id() != GetCurrentProcessId(), "Invalid child identity");
    child->terminate();
    require(co_await child->wait() == 1, "Explicit termination failed");
    child.reset();
    child = std::make_unique<agent::DesktopProcess>(loop.executor(), user, executable, pipe.name());
    auto observed = std::make_shared<asio::windows::object_handle>(loop.executor());
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child->id());
    require(handle != nullptr, "Cannot observe contained process");
    observed->assign(handle);
    child.reset(); // Job close must terminate the child even without a wait owner.
    asio::steady_timer deadline(loop.executor(), std::chrono::seconds(3));
    deadline.async_wait([observed](std::error_code error) { if (!error) { observed->cancel(); } });
    co_await observed->async_wait(asio::use_awaitable);
    deadline.cancel();
    DWORD code = STILL_ACTIVE;
    require(GetExitCodeProcess(observed->native_handle(), &code) && code != STILL_ACTIVE, "Job destruction leaked a child");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        asio::io_context io;
        ruvia::blocking_pool blockingPool{{.thread_count_ = 1, .queue_capacity_ = 4}};
        auto attachment = ruvia::attach_event_loop(io, {.blocking_pool_ = &blockingPool});
        std::exception_ptr failure;
        asio::co_spawn(attachment.loop().executor(), exercise(attachment.loop(), argv[1]), [&](std::exception_ptr error) {
            failure = error;
            attachment.stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Desktop user binding, invalid launch rejection, asynchronous exit and job cleanup passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
