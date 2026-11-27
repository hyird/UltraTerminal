#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include "platform/platform.transport.h"
#include "platform/windows/handle.types.h"
#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::agent {
namespace {
std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed) != needed) {
        return {};
    }
    return out;
}

struct SharedCaptureBudget {
    std::mutex mutex;
    std::size_t budget = 0;
    std::size_t used = 0;
    bool truncated = false;
    std::string stdoutData;
    std::string stderrData;

    void append(bool isStderr, const char* data, std::size_t count) {
        std::lock_guard lock(mutex);
        if (used < budget) {
            const std::size_t take = std::min(count, budget - used);
            auto& target = isStderr ? stderrData : stdoutData;
            target.append(data, take);
            used += take;
            if (take < count) {
                truncated = true;
            }
        } else {
            truncated = true;
        }
    }
};

void drainPipe(HANDLE pipe, bool isStderr, SharedCaptureBudget& sink) {
    std::array<char, 4096> buffer{};
    DWORD readBytes = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &readBytes, nullptr) && readBytes > 0) {
        sink.append(isStderr, buffer.data(), static_cast<std::size_t>(readBytes));
    }
}
} // namespace

wire::ExecCommandResult runPlatformExec(const wire::ExecCommandRequest& request) {
    wire::ExecCommandResult result;
    result.set_request_id(request.request_id() ? request.request_id() : 1);
    if (!protocol::validExecCommandRequest(request)) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_INVALID_REQUEST);
        result.set_exit_code(-1);
        return result;
    }

    const std::wstring wideCommand = utf8ToWide(request.command());
    if (wideCommand.empty()) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_INVALID_REQUEST);
        result.set_exit_code(-1);
        return result;
    }

    std::wstring wideCwd;
    if (!request.cwd().empty()) {
        wideCwd = utf8ToWide(request.cwd());
        if (wideCwd.empty()) {
            result.set_success(false);
            result.set_error(wire::EXEC_ERROR_INVALID_REQUEST);
            result.set_exit_code(-1);
            return result;
        }
        const DWORD attrs = GetFileAttributesW(wideCwd.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            result.set_success(false);
            result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
            result.set_exit_code(-1);
            return result;
        }
    }

    SECURITY_ATTRIBUTES inheritAttr{};
    inheritAttr.nLength = sizeof(inheritAttr);
    inheritAttr.bInheritHandle = TRUE;

    HANDLE rawOutRead = nullptr;
    HANDLE rawOutWrite = nullptr;
    HANDLE rawErrRead = nullptr;
    HANDLE rawErrWrite = nullptr;
    if (!CreatePipe(&rawOutRead, &rawOutWrite, &inheritAttr, 0)) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    UniqueHandle outRead(rawOutRead);
    UniqueHandle outWrite(rawOutWrite);
    (void)SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&rawErrRead, &rawErrWrite, &inheritAttr, 0)) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    UniqueHandle errRead(rawErrRead);
    UniqueHandle errWrite(rawErrWrite);
    (void)SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    UniqueHandle nullIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritAttr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }

    std::array<wchar_t, MAX_PATH> sysDir{};
    const UINT sysLen = GetSystemDirectoryW(sysDir.data(), static_cast<UINT>(sysDir.size()));
    if (sysLen == 0 || sysLen >= sysDir.size()) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    const std::wstring cmdExe = std::wstring(sysDir.data(), sysLen) + L"\\cmd.exe";
    std::wstring commandLine = L"\"" + cmdExe + L"\" /d /s /c \"" + wideCommand + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullIn.get();
    startup.hStdOutput = outWrite.get();
    startup.hStdError = errWrite.get();

    PROCESS_INFORMATION procInfo{};
    if (!CreateProcessW(cmdExe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        wideCwd.empty() ? nullptr : wideCwd.c_str(), &startup, &procInfo)) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    UniqueHandle childProcess(procInfo.hProcess);
    UniqueHandle childThread(procInfo.hThread);

    // Close parent's pipe write handles immediately after child creation.
    outWrite.reset();
    errWrite.reset();
    nullIn.reset();

    if (!AssignProcessToJobObject(job.get(), childProcess.get()) || ResumeThread(childThread.get()) == static_cast<DWORD>(-1)) {
        (void)TerminateProcess(childProcess.get(), 1);
        (void)WaitForSingleObject(childProcess.get(), 5000);
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    childThread.reset();

    SharedCaptureBudget sink;
    sink.budget = request.max_output_bytes()
                      ? std::min<std::size_t>(request.max_output_bytes(), protocol::kMaximumExecOutputBytes)
                      : protocol::kDefaultExecOutputBytes;

    std::jthread outReader(drainPipe, outRead.get(), false, std::ref(sink));
    std::jthread errReader(drainPipe, errRead.get(), true, std::ref(sink));

    const std::uint32_t timeoutMs = request.timeout_ms()
                                        ? std::clamp(request.timeout_ms(), protocol::kMinimumExecTimeoutMs, protocol::kMaximumExecTimeoutMs)
                                        : protocol::kDefaultExecTimeoutMs;
    const DWORD waitStatus = WaitForSingleObject(childProcess.get(), timeoutMs);
    bool timedOut = false;
    if (waitStatus == WAIT_TIMEOUT) {
        timedOut = true;
        (void)TerminateJobObject(job.get(), 1);
        (void)WaitForSingleObject(childProcess.get(), 5000);
    }

    DWORD exitCode = 1;
    (void)GetExitCodeProcess(childProcess.get(), &exitCode);
    job.reset();
    outReader.join();
    errReader.join();

    result.set_success(!timedOut);
    result.set_error(timedOut ? wire::EXEC_ERROR_TIMEOUT : wire::EXEC_ERROR_NONE);
    result.set_exit_code(timedOut ? -1 : static_cast<std::int32_t>(exitCode));
    result.set_stdout_data(std::move(sink.stdoutData));
    result.set_stderr_data(std::move(sink.stderrData));
    result.set_truncated(sink.truncated);
    result.set_timed_out(timedOut);
    return result;
}
} // namespace ultraterimal::agent
