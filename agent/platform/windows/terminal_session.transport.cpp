#include <algorithm>
#include <array>
#include <asio/buffer.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/windows/stream_handle.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "platform/platform.transport.h"
#include "platform/windows/handle.types.h"

namespace ultraterimal::agent {
namespace {
std::atomic<std::uint64_t> nextTerminalPipe{1};

void requireWin32(BOOL success, const char* operation) {
    if (!success) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}

void requireHresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}

std::pair<UniqueHandle, UniqueHandle> createOverlappedPipePair(bool agentWritesToChild) {
    const auto id = nextTerminalPipe.fetch_add(1, std::memory_order_relaxed);
    const std::wstring name = L"\\\\.\\pipe\\Ultraterimal-pty-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id);
    const DWORD serverMode = (agentWritesToChild ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
    UniqueHandle server(CreateNamedPipeW(name.c_str(), serverMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr));
    if (!server) {
        requireWin32(FALSE, "CreateNamedPipeW");
    }
    const DWORD clientAccess = agentWritesToChild ? GENERIC_READ : GENERIC_WRITE;
    UniqueHandle client(CreateFileW(name.c_str(), clientAccess, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!client) {
        requireWin32(FALSE, "CreateFileW pipe");
    }
    return {std::move(server), std::move(client)};
}

class WindowsTerminal final : public PlatformTerminal {
  public:
    explicit WindowsTerminal(ruvia::event_loop loop)
        : input_(loop.executor()), output_(loop.executor()) {
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        create_ = reinterpret_cast<decltype(&CreatePseudoConsole)>(GetProcAddress(kernel, "CreatePseudoConsole"));
        resize_ = reinterpret_cast<decltype(&ResizePseudoConsole)>(GetProcAddress(kernel, "ResizePseudoConsole"));
        close_ = reinterpret_cast<decltype(&ClosePseudoConsole)>(GetProcAddress(kernel, "ClosePseudoConsole"));
        if (!create_ || !resize_ || !close_) {
            throw std::runtime_error("ConPTY is unavailable");
        }

        auto [agentWrite, conptyRead] = createOverlappedPipePair(true);
        auto [agentRead, conptyWrite] = createOverlappedPipePair(false);

        requireHresult(create_({80, 25}, conptyRead.get(), conptyWrite.get(), 0, &console_), "CreatePseudoConsole");
        conptyRead.reset();
        conptyWrite.reset();

        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<std::byte> attrStorage(attrSize);
        auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
        requireWin32(InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize), "InitializeProcThreadAttributeList");
        struct AttrGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~AttrGuard() { DeleteProcThreadAttributeList(list); }
        } attrGuard{attrList};
        requireWin32(UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_, sizeof(HPCON), nullptr, nullptr), "UpdateProcThreadAttribute");

        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) {
            requireWin32(FALSE, "CreateJobObjectW");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        requireWin32(SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "SetInformationJobObject");

        std::array<wchar_t, MAX_PATH> sysDir{};
        const auto len = GetSystemDirectoryW(sysDir.data(), static_cast<UINT>(sysDir.size()));
        if (!len || len >= sysDir.size()) {
            throw std::runtime_error("Cannot resolve Windows system directory");
        }
        const std::wstring exe = std::wstring(sysDir.data(), len) + L"\\cmd.exe";
        std::wstring cmd = L"\"" + exe + L"\"";
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = attrList;
        PROCESS_INFORMATION proc{};
        requireWin32(CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, sysDir.data(), &startup.StartupInfo, &proc),
                     "CreateProcessW");
        process_.reset(proc.hProcess);
        UniqueHandle thread(proc.hThread);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            const auto err = GetLastError();
            TerminateProcess(process_.get(), err);
            throw std::system_error(static_cast<int>(err), std::system_category(), "AssignProcessToJobObject");
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            requireWin32(FALSE, "ResumeThread");
        }

        output_.assign(agentWrite.release());
        input_.assign(agentRead.release());
    }

    ~WindowsTerminal() override { close(); }

    asio::awaitable<std::string> read() override {
        std::array<char, 16384> buffer{};
        asio::error_code ec;
        const auto count = co_await input_.async_read_some(asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, ec));
        if (ec || count == 0) {
            co_return std::string{};
        }
        co_return std::string(buffer.data(), count);
    }

    asio::awaitable<void> write(std::string_view bytes) override {
        if (bytes.empty()) {
            co_return;
        }
        std::string copy(bytes);
        co_await asio::async_write(output_, asio::buffer(copy), asio::use_awaitable);
    }

    void resize(std::uint32_t columns, std::uint32_t rows) override {
        if (console_ && !closed_) {
            const SHORT c = static_cast<SHORT>(std::clamp<std::uint32_t>(columns, 1, 1000));
            const SHORT r = static_cast<SHORT>(std::clamp<std::uint32_t>(rows, 1, 1000));
            (void)resize_(console_, {c, r});
        }
    }

    std::uint32_t waitExit() override {
        if (!process_) {
            return 0;
        }
        (void)WaitForSingleObject(process_.get(), 2000);
        DWORD code = 0;
        if (!GetExitCodeProcess(process_.get(), &code) || code == STILL_ACTIVE) {
            return 0;
        }
        return code;
    }

    void close() noexcept override {
        if (closed_) {
            return;
        }
        closed_ = true;
        asio::error_code ignored;
        input_.cancel(ignored);
        input_.close(ignored);
        output_.cancel(ignored);
        output_.close(ignored);
        if (console_) {
            close_(console_);
            console_ = nullptr;
        }
        if (job_) {
            TerminateJobObject(job_.get(), 0);
            job_.reset();
        }
        if (process_) {
            (void)WaitForSingleObject(process_.get(), 500);
            process_.reset();
        }
    }

  private:
    asio::windows::stream_handle input_;
    asio::windows::stream_handle output_;
    HPCON console_ = nullptr;
    UniqueHandle job_;
    UniqueHandle process_;
    decltype(&CreatePseudoConsole) create_ = nullptr;
    decltype(&ResizePseudoConsole) resize_ = nullptr;
    decltype(&ClosePseudoConsole) close_ = nullptr;
    bool closed_ = false;
};
} // namespace

std::unique_ptr<PlatformTerminal> PlatformTerminal::spawn(ruvia::event_loop loop) {
    return std::make_unique<WindowsTerminal>(std::move(loop));
}
} // namespace ultraterimal::agent
