#include "platform/windows/terminal.transport.h"

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <windows.h>

#include "platform/windows/handle.types.h"

namespace ultraterimal::agent {
namespace {
void require(BOOL success, const char* operation) {
    if (!success) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}
void requireResult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}
void rejectSystemIdentity() {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "OpenProcessToken");
    UniqueHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> storage(size);
    require(GetTokenInformation(token.get(), TokenUser, storage.data(), size, &size), "TokenUser");
    const auto user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    if (IsWellKnownSid(user->User.Sid, WinLocalSystemSid)) {
        throw std::runtime_error("Local terminal diagnostic refuses the SYSTEM identity");
    }
}
class AttributeList {
  public:
    AttributeList() {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        require(InitializeProcThreadAttributeList(list_, 1, 0, &bytes), "InitializeProcThreadAttributeList");
    }
    ~AttributeList() { DeleteProcThreadAttributeList(list_); }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

  private:
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};
class PseudoConsole {
  public:
    PseudoConsole(HANDLE input, HANDLE output) {
        const auto kernel = GetModuleHandleW(L"kernel32.dll");
        const auto create = reinterpret_cast<decltype(&CreatePseudoConsole)>(GetProcAddress(kernel, "CreatePseudoConsole"));
        close_ = reinterpret_cast<decltype(&ClosePseudoConsole)>(GetProcAddress(kernel, "ClosePseudoConsole"));
        resize_ = reinterpret_cast<decltype(&ResizePseudoConsole)>(GetProcAddress(kernel, "ResizePseudoConsole"));
        if (!create || !close_ || !resize_) {
            throw std::runtime_error("ConPTY is not available");
        }
        requireResult(create({80, 25}, input, output, 0, &console_), "CreatePseudoConsole");
    }
    ~PseudoConsole() { close(); }
    PseudoConsole(const PseudoConsole&) = delete;
    PseudoConsole& operator=(const PseudoConsole&) = delete;
    HPCON get() const { return console_; }
    void resize() { requireResult(resize_(console_, {100, 30}), "ResizePseudoConsole"); }
    void close() noexcept {
        if (console_) {
            close_(console_);
            console_ = nullptr;
        }
    }

  private:
    HPCON console_ = nullptr;
    decltype(&ClosePseudoConsole) close_ = nullptr;
    decltype(&ResizePseudoConsole) resize_ = nullptr;
};
} // namespace
TerminalDiagnostic checkLocalTerminal() {
    rejectSystemIdentity();
    HANDLE rawRead = nullptr;
    HANDLE rawWrite = nullptr;
    require(CreatePipe(&rawRead, &rawWrite, nullptr, 0), "CreatePipe stdin");
    UniqueHandle inputRead(rawRead), inputWrite(rawWrite);
    require(CreatePipe(&rawRead, &rawWrite, nullptr, 0), "CreatePipe stdout");
    UniqueHandle outputRead(rawRead), outputWrite(rawWrite);
    std::string output;
    std::atomic<DWORD> readError = NO_ERROR;
    // Drain before creating/closing ConPTY, including teardown and overflow.
    std::jthread reader([&] {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(outputRead.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0) {
            try {
                if (output.size() + count <= 65536) {
                    output.append(buffer.data(), count);
                } else {
                    readError.store(ERROR_BUFFER_OVERFLOW);
                }
            } catch (...) {
                readError.store(ERROR_NOT_ENOUGH_MEMORY);
            }
        }
        const auto error = GetLastError();
        if (error != NO_ERROR && error != ERROR_BROKEN_PIPE) {
            readError.store(error);
        }
    });
    try {
        PseudoConsole console(inputRead.get(), outputWrite.get());
        inputRead.reset();
        outputWrite.reset();
        console.resize();
        AttributeList attributes;
        require(UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console.get(), sizeof(HPCON), nullptr, nullptr), "UpdateProcThreadAttribute");
        UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
        if (!job) {
            require(FALSE, "CreateJobObjectW");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "SetInformationJobObject");
        std::array<wchar_t, MAX_PATH> systemDirectory{};
        const auto length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
        if (!length || length >= systemDirectory.size()) {
            throw std::runtime_error("Cannot resolve the system directory");
        }
        const std::wstring executable = std::wstring(systemDirectory.data(), length) + L"\\cmd.exe";
        auto command = L"\"" + executable + L"\" /d /q /c echo ULTRATERIMAL_CONPTY_OK";
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        // Null standard handles force attachment to ConPTY even when the parent is redirected.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION process{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                               EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, systemDirectory.data(), &startup.StartupInfo, &process),
                "CreateProcessW");
        UniqueHandle child(process.hProcess), thread(process.hThread);
        if (!AssignProcessToJobObject(job.get(), child.get())) {
            const auto error = GetLastError();
            TerminateProcess(child.get(), error);
            WaitForSingleObject(child.get(), 5000);
            throw std::system_error(static_cast<int>(error), std::system_category(), "AssignProcessToJobObject");
        }
        if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            require(FALSE, "ResumeThread");
        }
        const auto wait = WaitForSingleObject(child.get(), 10000);
        if (wait != WAIT_OBJECT_0) {
            TerminateJobObject(job.get(), ERROR_TIMEOUT);
            WaitForSingleObject(child.get(), 5000);
            throw std::runtime_error("ConPTY diagnostic did not finish within 10 seconds");
        }
        DWORD exitCode = 0;
        require(GetExitCodeProcess(child.get(), &exitCode), "GetExitCodeProcess");
        job.reset();
        inputWrite.reset();
        console.close();
        reader.join();
        if (readError.load() != NO_ERROR) {
            throw std::system_error(static_cast<int>(readError.load()), std::system_category(), "ConPTY output");
        }
        return {exitCode, output.find("ULTRATERIMAL_CONPTY_OK") != std::string::npos};
    } catch (...) {
        inputRead.reset();
        inputWrite.reset();
        outputWrite.reset();
        // Console and child job were destroyed while the reader was still active.
        if (reader.joinable()) {
            reader.join();
        }
        throw;
    }
}
} // namespace ultraterimal::agent
