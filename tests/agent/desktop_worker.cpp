#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <ruvia/core/EventLoopAttachment.h>

#include "desktop_process.fixture.h"
#include "platform/windows/pipe.transport.h"
#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace {
using namespace ultraterimal;
using test::Child;
using test::require;
wire::Envelope message(std::uint64_t sequence) {
    wire::Envelope result;
    result.set_protocol_version(protocol::kVersion);
    result.set_session_id("worker-fixture");
    result.set_sequence(sequence);
    return result;
}
asio::awaitable<void> send(agent::NamedPipeTransport& pipe, const wire::Envelope& envelope) {
    auto bytes = protocol::encodeMessage(envelope);
    require(bool(bytes), "Fixture violated protocol");
    co_await pipe.send(std::move(*bytes));
}
asio::awaitable<void> exchange(agent::NamedPipeTransport& pipe, const Child& child, DWORD session, const std::string& mode) {
    co_await pipe.accept(child.id, session);
    auto start = message(1);
    start.mutable_desktop_start()->set_windows_session_id(mode == "binding" ? session + 1 : session);
    start.mutable_desktop_start()->set_write_control(mode == "writable" || mode == "probe_delayed_recover");
    start.mutable_desktop_start()->set_desktop_input_contract(start.desktop_start().write_control() ? 2 : 0);
    co_await send(pipe, start);
    protocol::DesktopPipeValidator incoming({"worker-fixture", session, start.desktop_start().write_control(), protocol::DesktopPipeSource::kDesktop});
    bool ready = false, configured = false, refreshed = false, firstFrame = false, cursorReceived = false;
    std::uint64_t videoGeneration = 0;
    bool closed = false;
    unsigned probes = 0;
    std::uint64_t probeSequence = 2;
    auto firstProbeAt = std::chrono::steady_clock::time_point{};
    while (!closed) {
        std::string bytes;
        try {
            bytes = co_await pipe.receive();
        } catch (const std::exception&) {
            closed = true;
            break;
        }
        auto packet = protocol::decodeMessage(bytes);
        require(packet && incoming.accept(*packet), "Desktop worker produced invalid IPC");
        if (packet->has_session_error()) {
            throw std::runtime_error("Unexpected worker error");
        }
        if (packet->has_desktop_ready()) {
            require(!ready && packet->sequence() == 1, "Invalid desktop readiness");
            ready = true;
        } else if (packet->has_video_config()) {
            require(ready && !configured && packet->video_config().width() > 0, "Missing desktop configuration");
            configured = true;
            videoGeneration = packet->generation();
        } else if (packet->has_cursor_update()) {
            require(ready && packet->channel_id() == "input", "Cursor must use the input channel after readiness");
            cursorReceived = true;
        } else if (packet->has_desktop_probe()) {
            require(ready && packet->channel_id() == "input" && packet->desktop_probe().nonce() == ++probes, "Invalid desktop liveness probe");
            if (probes == 1) {
                firstProbeAt = std::chrono::steady_clock::now();
            }
            if (mode == "probe_echo" && probes == 2) {
                require(packet->desktop_probe().accepted_response_nonce() == 1, "Worker did not report the timely accepted response");
            }
            if (mode == "probe_recover" || mode == "probe_delayed_recover") {
                require(!packet->desktop_probe().accepted_response_nonce(), "Worker reported a response it did not accept");
                require(probes != 1 || !packet->desktop_probe().reset_input(), "Fresh worker marked a spurious input reset");
                require(!packet->desktop_probe().reset_input(), "Delayed probes must not freeze input");
                if (mode == "probe_delayed_recover" && probes == 3) {
                    auto staleInput = message(++probeSequence);
                    staleInput.set_channel_id("input");
                    staleInput.set_generation(videoGeneration);
                    staleInput.mutable_input_event()->set_kind(wire::InputEvent::KEY);
                    staleInput.mutable_input_event()->set_code(65);
                    staleInput.mutable_input_event()->set_pressed(true);
                    co_await send(pipe, staleInput);
                }
            }
            if (mode != "probe_timeout" &&
                ((mode != "probe_recover" && mode != "probe_delayed_recover") || probes >= 4)) {
                if (mode == "probe_delayed_recover" && probes == 4) {
                    asio::steady_timer delay(co_await asio::this_coro::executor);
                    delay.expires_after(std::chrono::milliseconds(2500));
                    co_await delay.async_wait(asio::use_awaitable);
                }
                auto response = message(++probeSequence);
                response.set_channel_id("input");
                response.mutable_desktop_probe()->set_nonce(packet->desktop_probe().nonce());
                co_await send(pipe, response);
                if (mode == "probe_replay") {
                    response.set_sequence(++probeSequence);
                    co_await send(pipe, response);
                } else if (mode == "probe_echo" && probes == 2) {
                    auto close = message(++probeSequence);
                    close.mutable_session_close()->set_reason("probes_verified");
                    co_await send(pipe, close);
                } else if ((mode == "probe_recover" || mode == "probe_delayed_recover") && probes == 4) {
                    auto close = message(++probeSequence);
                    close.mutable_session_close()->set_reason("recovered");
                    co_await send(pipe, close);
                }
            }
        } else if (packet->has_video_frame()) {
            require(configured && !packet->video_frame().data().empty(), "Missing desktop pixels");
            if (!firstFrame) {
                require(packet->video_frame().key_frame(), "First worker frame is not an IDR");
                firstFrame = true;
                if (mode == "disconnect") {
                    pipe.cancel();
                    co_return;
                }
                auto command = message(2);
                command.set_channel_id("input");
                if (mode == "readonly_input" || mode == "writable") {
                    command.set_generation(videoGeneration);
                }
                if (mode == "readonly_input") {
                    command.mutable_input_event()->set_kind(wire::InputEvent::KEY);
                    command.mutable_input_event()->set_code(65);
                    command.mutable_input_event()->set_pressed(true);
                } else if (mode == "writable") {
                    command.mutable_input_event()->set_kind(wire::InputEvent::RELEASE_ALL);
                } else {
                    command.mutable_request_refresh();
                }
                co_await send(pipe, command);
                if (mode == "writable") {
                    auto close = message(3);
                    close.mutable_session_close()->set_reason("completed");
                    co_await send(pipe, close);
                }
            } else if (mode == "normal" && !refreshed && packet->video_frame().key_frame()) {
                refreshed = true;
                auto close = message(3);
                close.mutable_session_close()->set_reason("completed");
                co_await send(pipe, close);
            }
        } else {
            throw std::runtime_error("Unexpected desktop IPC message");
        }
    }
    if (mode == "normal") {
        require(ready && firstFrame && refreshed && cursorReceived && closed, "Worker cursor/refresh/close path incomplete");
    }
    if (mode == "binding") {
        require(!ready, "Worker accepted wrong Windows session");
    }
    if (mode == "readonly_input") {
        require(firstFrame && closed, "Readonly input was not rejected");
    }
    if (mode == "writable") {
        require(ready && firstFrame && closed, "Writable input/close path incomplete");
    }
    if (mode == "probe_timeout") {
        const auto elapsed = std::chrono::steady_clock::now() - firstProbeAt;
        require(probes >= 1 && closed && elapsed >= std::chrono::seconds(57) && elapsed < std::chrono::seconds(62), "Missing probe response did not retain then expire the worker");
    } else if (mode == "probe_replay") {
        require(probes == 1 && closed && std::chrono::steady_clock::now() - firstProbeAt < std::chrono::seconds(2), "Duplicate probe response did not fail closed");
    } else if (mode == "probe_echo") {
        require(probes == 2 && closed, "Successful probe replies did not preserve the worker");
    } else if (mode == "probe_recover") {
        require(probes == 4 && closed && std::chrono::steady_clock::now() - firstProbeAt >= std::chrono::seconds(6), "Fresh probe did not recover after transient packet loss");
    } else if (mode == "probe_delayed_recover") {
        const auto elapsed = std::chrono::steady_clock::now() - firstProbeAt;
        if (!(probes >= 4 && closed && elapsed >= std::chrono::seconds(6) && elapsed < std::chrono::seconds(15))) {
            std::cerr << "delayed_probe_count=" << probes << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                      << " pipe_closed=" << (closed ? "yes" : "no") << '\n';
            throw std::runtime_error("Delayed reset probe did not recover after transient packet loss");
        }
    }
}
void requireFailureEvent(DWORD process, const wchar_t* expected) {
    struct Log {
        HANDLE handle = OpenEventLogW(nullptr, L"Application");
        ~Log() {
            if (handle) {
                CloseEventLog(handle);
            }
        }
    } log;
    require(log.handle != nullptr, "Cannot read desktop diagnostic events");
    std::vector<std::byte> buffer(65536);
    const auto identity = L"pid=" + std::to_wstring(process) + L" ";
    for (unsigned batch = 0; batch < 32; ++batch) {
        DWORD bytes = 0, needed = 0;
        if (!ReadEventLogW(log.handle, EVENTLOG_BACKWARDS_READ | EVENTLOG_SEQUENTIAL_READ, 0,
                          buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, &needed)) {
            break;
        }
        for (DWORD offset = 0; offset < bytes;) {
            const auto* record = reinterpret_cast<const EVENTLOGRECORD*>(buffer.data() + offset);
            require(record->Length >= sizeof(EVENTLOGRECORD) && record->Length <= bytes - offset, "Invalid event record");
            const auto* source = reinterpret_cast<const wchar_t*>(record + 1);
            if (record->EventID == 1001 && record->NumStrings == 4 && std::wstring_view(source) == L"UltraterimalDesktop") {
                const auto* context = reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::byte*>(record) + record->StringOffset);
                const auto* stage = context + std::wcslen(context) + 1;
                const auto* detail = stage + std::wcslen(stage) + 1;
                if (std::wstring_view(context).starts_with(identity)) {
                    require(std::wstring_view(stage) == L"input_validation", "Diagnostic lost original failing stage");
                    require(std::wstring_view(detail) == expected, "Diagnostic lost original error during cancellation");
                    require(std::wstring_view(detail + std::wcslen(detail) + 1) == L"worker-fixture", "Diagnostic lost remote session identity");
                    return;
                }
            }
            offset += record->Length;
        }
    }
    throw std::runtime_error("Desktop failure event was not recorded");
}
void run(const std::wstring& executable, const std::string& mode) {
    DWORD session = 0;
    require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session != 0, "Interactive test session required");
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    agent::NamedPipeTransport pipe(io.get_executor(), agent::currentLogonSid());
    Child child(executable, pipe.name(), mode == "process_session" ? session + 1 : session);
    if (mode == "process_session") {
        child.finish(false);
        std::cout << mode << " desktop process passed\n";
        return;
    }
    asio::steady_timer deadline(io);
    bool timedOut = false;
    deadline.expires_after(mode == "probe_timeout" ? std::chrono::seconds(65) : mode == "probe_delayed_recover" ? std::chrono::seconds(15)
                                                                                                                : std::chrono::seconds(10));
    deadline.async_wait([&](std::error_code error) { if (!error) { timedOut = true; pipe.cancel(); } });
    std::exception_ptr failure;
    asio::co_spawn(io, exchange(pipe, child, session, mode), [&](std::exception_ptr error) {
        failure = error;
        deadline.cancel();
        pipe.cancel();
        attachment.stop();
    });
    attachment.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    require(!timedOut, "Desktop worker timed out");
    child.finish(mode == "normal" || mode == "writable" || mode == "probe_echo" || mode == "probe_recover" || mode == "probe_delayed_recover");
    if (mode == "readonly_input") {
        requireFailureEvent(child.id, L"Desktop command violates IPC contract");
    } else if (mode == "probe_replay") {
        requireFailureEvent(child.id, L"Replayed desktop probe response");
    }
    std::cout << mode << " desktop process passed\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        return 2;
    }
    std::string currentMode;
    try {
        require(SetThreadExecutionState(ES_DISPLAY_REQUIRED) != 0, "Cannot wake test display");
        for (const std::string mode : {"normal", "binding", "readonly_input", "disconnect", "writable", "process_session", "probe_echo", "probe_recover", "probe_delayed_recover", "probe_timeout", "probe_replay"}) {
            if (argc == 3 && std::wstring(mode.begin(), mode.end()) != argv[2]) {
                continue;
            }
            currentMode = mode;
            run(argv[1], mode);
        }
        require(!currentMode.empty(), "Unknown desktop worker test mode");
    } catch (const std::exception& error) {
        std::cerr << currentMode << ": " << error.what() << '\n';
        return 1;
    }
}
