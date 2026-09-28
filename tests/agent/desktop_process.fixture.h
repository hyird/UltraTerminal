#pragma once
#include <stdexcept>
#include <string>

#include <windows.h>

#include "platform/windows/handle.types.h"
#include "ultraterimal/protocol/desktop_failure.protocol.h"
namespace ultraterimal::test {
inline void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
class Child {
  public:
    agent::UniqueHandle job, process;
    DWORD id = 0;
    DWORD sessionId = 0;
    Child(const std::wstring& executable, const std::wstring& pipe, DWORD session) : sessionId(session) {
        job.reset(CreateJobObjectW(nullptr, nullptr));
        require(bool(job), "Cannot create desktop worker job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "Cannot contain desktop worker");
        std::wstring command = L"\"" + executable + L"\" --desktop --session " + std::to_wstring(session) + L" --pipe " + pipe + L" --parent " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION information{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information) != FALSE, "Cannot launch desktop worker");
        process.reset(information.hProcess);
        agent::UniqueHandle thread(information.hThread);
        id = information.dwProcessId;
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 1);
            throw std::runtime_error("Cannot assign desktop worker job");
        }
        require(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "Cannot resume desktop worker");
    }
    void finish(bool success) {
        require(WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0, "Desktop worker did not exit");
        DWORD code = STILL_ACTIVE;
        require(GetExitCodeProcess(process.get(), &code) != FALSE, "Cannot read desktop worker exit status");
        require(success ? code == 0 : code == 1 || protocol::desktopFailureFromExitCode(code, sessionId, id).has_value(), "Unexpected desktop worker exit status");
    }
};

} // namespace ultraterimal::test
