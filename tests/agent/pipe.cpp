#include <aclapi.h>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_future.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <sddl.h>
#include <stdexcept>
#include <string>

#include <windows.h>

#include "platform/windows/handle.types.h"
#include "platform/windows/pipe.transport.h"

namespace {
using ultraterimal::agent::NamedPipeTransport;
using ultraterimal::agent::UniqueHandle;
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Named pipe invariant failed");
    }
}
std::string payload() {
    std::string bytes(1024 * 1024, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<char>(i % 251);
    }
    return bytes;
}
struct Child {
    UniqueHandle job, process;
    DWORD id = 0;
    Child(const std::wstring& pipe, const std::wstring& mode) {
        std::wstring executable(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(size && size < executable.size());
        executable.resize(size);
        std::wstring command = L"\"" + executable + L"\" --child " + pipe + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + mode;
        job.reset(CreateJobObjectW(nullptr, nullptr));
        require(bool(job));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information) != FALSE);
        process.reset(information.hProcess);
        UniqueHandle thread(information.hThread);
        id = information.dwProcessId;
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1);
            throw std::runtime_error("Cannot contain pipe fixture child");
        }
        require(ResumeThread(thread.get()) != static_cast<DWORD>(-1));
    }
    void finish() {
        require(WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0);
        DWORD code = 1;
        require(GetExitCodeProcess(process.get(), &code) && code == 0);
    }
};
asio::awaitable<void> childExchange(NamedPipeTransport& pipe, bool echo, bool idle) {
    if (echo) {
        co_await pipe.send(payload());
        require((co_await pipe.receive()) == "accepted");
    } else if (idle) {
        co_await pipe.send("idle");
        require((co_await pipe.receive()) == "accepted");
    } else {
        bool closed = false;
        try {
            (void)co_await pipe.receive();
        } catch (const std::exception&) {
            closed = true;
        }
        require(closed);
    }
}
int child(const wchar_t* name, DWORD parent, const std::wstring& mode) {
    asio::io_context io;
    if (mode == L"bad_length" || mode == L"partial_header") {
        UniqueHandle raw(CreateFileW(name, FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        require(bool(raw));
        ULONG server = 0;
        require(GetNamedPipeServerProcessId(raw.get(), &server) && server == parent);
        const unsigned char length[]{1, 0, 0, 1}; // 16 MiB + 1, no payload allocation required.
        DWORD written = 0;
        const DWORD count = mode == L"partial_header" ? 1 : sizeof(length);
        require(WriteFile(raw.get(), length, count, &written, nullptr) && written == count);
        char byte = 0;
        require(!ReadFile(raw.get(), &byte, 1, &written, nullptr));
        return 0;
    }
    if (mode == L"bad_server") {
        bool rejected = false;
        try {
            (void)NamedPipeTransport::connect(io.get_executor(), name, GetCurrentProcessId());
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected);
        return 0;
    }
    auto pipe = NamedPipeTransport::connect(io.get_executor(), name, parent);
    if (mode == L"idle") {
        Sleep(10500);
    }
    auto result = asio::co_spawn(io, childExchange(*pipe, mode == L"echo", mode == L"idle"), asio::use_future);
    io.run();
    result.get();
    return 0;
}
asio::awaitable<void> serve(NamedPipeTransport& pipe, DWORD process, DWORD session, const std::wstring& mode) {
    bool failed = false;
    try {
        co_await pipe.accept(mode == L"bad_client" ? GetCurrentProcessId() : process, mode == L"bad_session" ? 0xffffffff : session);
        if (mode == L"echo") {
            require((co_await pipe.receive()) == payload());
            co_await pipe.send("accepted");
        } else if (mode == L"idle") {
            require((co_await pipe.receive()) == "idle");
            co_await pipe.send("accepted");
        } else {
            (void)co_await pipe.receive();
        }
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed == (mode != L"echo" && mode != L"idle"));
}
void verifySecurity() {
    asio::io_context io;
    const auto logon = ultraterimal::agent::currentLogonSid();
    NamedPipeTransport server(io.get_executor(), logon);
    UniqueHandle duplicate(CreateNamedPipeW(server.name().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024, 1024, 0, nullptr));
    require(!duplicate);
    UniqueHandle client(CreateFileW(server.name().c_str(), READ_CONTROL, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    require(bool(client));
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL acl = nullptr;
    require(GetSecurityInfo(client.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS);
    struct Cleanup {
        PSECURITY_DESCRIPTOR value;
        ~Cleanup() { LocalFree(value); }
    } cleanup{descriptor};
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED));
    require(acl && acl->AceCount == 2);
    void* entry = nullptr;
    require(GetAce(acl, 1, &entry) != FALSE);
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
    require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == (SYNCHRONIZE | FILE_READ_DATA | FILE_WRITE_DATA));
    LPWSTR sid = nullptr;
    require(ConvertSidToStringSidW(const_cast<DWORD*>(&ace->SidStart), &sid) != FALSE);
    const bool matches = logon == sid;
    LocalFree(sid);
    require(matches);
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 5 && std::wstring(argv[1]) == L"--child") {
            return child(argv[2], std::stoul(argv[3]), argv[4]);
        }
        require(argc == 1);
        verifySecurity();
        DWORD session = 0;
        require(ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE);
        for (const std::wstring mode : {L"echo", L"bad_client", L"bad_session", L"bad_server", L"bad_length", L"partial_header", L"idle", L"cancel_read", L"cancel_accept"}) {
            asio::io_context io;
            NamedPipeTransport server(io.get_executor(), ultraterimal::agent::currentLogonSid());
            asio::steady_timer timer(io);
            std::unique_ptr<Child> process;
            if (mode != L"cancel_accept") {
                process = std::make_unique<Child>(server.name(), mode);
            }
            if (mode == L"bad_server") {
                process->finish();
                continue;
            }
            bool watchdogExpired = false;
            asio::steady_timer cancelTimer(io);
            timer.expires_after(std::chrono::seconds(14));
            timer.async_wait([&](std::error_code error) {
                if (!error) {
                    watchdogExpired = true;
                    server.cancel();
                }
            });
            if (mode == L"cancel_read" || mode == L"cancel_accept") {
                cancelTimer.expires_after(std::chrono::milliseconds(150));
                cancelTimer.async_wait([&](std::error_code error) { if (!error) { server.cancel(); } });
            }
            std::exception_ptr serveError;
            asio::co_spawn(io, serve(server, process ? process->id : GetCurrentProcessId(), session, mode),
                           [&](std::exception_ptr error) { serveError = error; timer.cancel(); });
            io.run();
            if (serveError) {
                std::rethrow_exception(serveError);
            }
            require(!watchdogExpired);
            if (process) {
                process->finish();
            }
        }
        std::cout << "Protected pipe ACL, client/server PID and session binding, binary framing, limits, partial-header timeout, idle connection and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
