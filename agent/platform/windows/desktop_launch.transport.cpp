#include "desktop_launch.transport.h"

#include <algorithm>
#include <asio/use_awaitable.hpp>
#include <asio/windows/object_handle.hpp>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <userenv.h>
#include <vector>

#include <windows.h>

#include <sddl.h>
#include <wtsapi32.h>

#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
void check(BOOL success, const char* operation) {
    if (!success) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}
std::vector<BYTE> information(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536) {
        throw std::runtime_error("Invalid desktop token information");
    }
    std::vector<BYTE> bytes(size);
    check(GetTokenInformation(token, kind, bytes.data(), size, &size), "Read desktop token");
    return bytes;
}
void requireActive(DWORD session) {
    struct Buffer {
        LPWSTR value = nullptr;
        ~Buffer() { WTSFreeMemory(value); }
    } buffer;
    DWORD bytes = 0;
    check(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSConnectState, &buffer.value, &bytes), "Read desktop session state");
    if (bytes != sizeof(WTS_CONNECTSTATE_CLASS) || *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(buffer.value) != WTSActive) {
        throw std::runtime_error("Desktop session is not active");
    }
}
std::wstring logonSid(HANDLE token) {
    const auto bytes = information(token, TokenGroups);
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(bytes.data());
    for (DWORD index = 0; index < groups->GroupCount; ++index) {
        if ((groups->Groups[index].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            struct Text {
                LPWSTR value = nullptr;
                ~Text() { LocalFree(value); }
            } text;
            check(ConvertSidToStringSidW(groups->Groups[index].Sid, &text.value), "Encode desktop logon SID");
            return text.value;
        }
    }
    throw std::runtime_error("Desktop token has no logon SID");
}
class SystemPrivileges {
  public:
    SystemPrivileges() {
        HANDLE existing = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &existing)) {
            CloseHandle(existing);
            throw std::runtime_error("Desktop launcher cannot replace an impersonated identity");
        }
        if (GetLastError() != ERROR_NO_TOKEN) {
            check(FALSE, "Inspect launcher thread token");
        }
        check(ImpersonateSelf(SecurityImpersonation), "Select launcher thread identity");
        try {
            HANDLE raw = nullptr;
            check(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, TRUE, &raw), "Open launcher thread privileges");
            UniqueHandle token(raw);
            for (const auto name : {SE_TCB_NAME, SE_ASSIGNPRIMARYTOKEN_NAME, SE_INCREASE_QUOTA_NAME}) {
                TOKEN_PRIVILEGES privileges{};
                privileges.PrivilegeCount = 1;
                check(LookupPrivilegeValueW(nullptr, name, &privileges.Privileges[0].Luid), "Resolve launcher privilege");
                privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                check(AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr), "Enable launcher privilege");
                if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                    check(FALSE, "Missing launcher privilege");
                }
            }
        } catch (...) {
            if (!RevertToSelf()) {
                std::terminate();
            }
            throw;
        }
    }
    ~SystemPrivileges() {
        if (!RevertToSelf()) {
            std::terminate();
        }
    }
};
void requireConsole(DWORD session) {
    if (WTSGetActiveConsoleSessionId() != session) {
        throw std::runtime_error("Target is not the current console session");
    }
}
} // namespace
struct DesktopIdentity::State {
    UniqueHandle token;
    std::wstring sid;
    DWORD session = 0;
    bool current = false;
    explicit State(DWORD selected) : session(selected) {
        if (!session || session == 0xffffffff) {
            throw std::invalid_argument("Invalid desktop session");
        }
        DWORD processSession = 0;
        check(ProcessIdToSessionId(GetCurrentProcessId(), &processSession), "Read process session");
        HANDLE raw = nullptr;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw), "Open desktop launcher token");
        token.reset(raw);
        const auto userBytes = information(token.get(), TokenUser);
        const auto user = reinterpret_cast<const TOKEN_USER*>(userBytes.data())->User.Sid;
        if (processSession) {
            if (processSession != session) {
                throw std::runtime_error("Interactive launcher cannot select another session");
            }
            if (IsWellKnownSid(user, WinLocalServiceSid) || IsWellKnownSid(user, WinNetworkServiceSid) || IsWellKnownSid(user, WinLocalSystemSid)) {
                throw std::runtime_error("Only the Session 0 service may launch a privileged desktop");
            }
            current = true;
            requireActive(session);
            sid = agent::logonSid(token.get());
        } else {
            if (!IsWellKnownSid(user, WinLocalSystemSid)) {
                throw std::runtime_error("Desktop service must run as LocalSystem");
            }
            requireConsole(session);
            SystemPrivileges privileges;
            HANDLE primary = nullptr;
            check(DuplicateTokenEx(token.get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_SESSIONID | TOKEN_ADJUST_DEFAULT,
                                   nullptr, SecurityImpersonation, TokenPrimary, &primary),
                  "Create desktop system token");
            token.reset(primary);
            check(SetTokenInformation(token.get(), TokenSessionId, &session, sizeof(session)), "Bind desktop system token to console");
            sid = L"S-1-5-18";
        }
        const auto sessionBytes = information(token.get(), TokenSessionId);
        if (*reinterpret_cast<const DWORD*>(sessionBytes.data()) != session) {
            throw std::runtime_error("Desktop token session mismatch");
        }
    }
};
struct DesktopProcess::State {
    UniqueHandle job;
    UniqueHandle process;
    asio::any_io_executor executor;
    DWORD id = 0;
    bool waiting = false;
    State(asio::any_io_executor owner) : executor(std::move(owner)) {}
    ~State() { job.reset(); }
    static asio::awaitable<std::uint32_t> waitOwned(std::shared_ptr<State> self) {
        if (self->waiting || !self->executor) {
            throw std::logic_error("Overlapping desktop process waits");
        }
        self->waiting = true;
        struct Reset {
            bool& flag;
            ~Reset() { flag = false; }
        } reset{self->waiting};
        HANDLE duplicate = nullptr;
        check(DuplicateHandle(GetCurrentProcess(), self->process.get(), GetCurrentProcess(), &duplicate, SYNCHRONIZE, FALSE, 0), "Observe desktop process");
        UniqueHandle owned(duplicate);
        asio::windows::object_handle observer(self->executor, owned.get());
        owned.release();
        co_await observer.async_wait(asio::use_awaitable);
        DWORD code = STILL_ACTIVE;
        check(GetExitCodeProcess(self->process.get(), &code), "Read desktop process exit");
        co_return code;
    }
};
DesktopIdentity::DesktopIdentity(std::uint32_t session) : state_(std::make_unique<State>(session)) {}
std::uint32_t currentDesktopSession() {
    DWORD session = 0;
    check(ProcessIdToSessionId(GetCurrentProcessId(), &session), "Read launcher session");
    return session ? session : WTSGetActiveConsoleSessionId();
}
DesktopIdentity::~DesktopIdentity() = default;
const std::wstring& DesktopIdentity::pipeSid() const { return state_->sid; }
std::uint32_t DesktopIdentity::session() const { return state_->session; }
std::uint32_t DesktopIdentity::sendSecureAttention() const {
    if (state_->current || WTSGetActiveConsoleSessionId() != state_->session) {
        return ERROR_ACCESS_DENIED;
    }
    // Preserve an explicitly configured policy, including a domain denial.
    constexpr auto key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
    constexpr auto name = L"SoftwareSASGeneration";
    DWORD policy = 0, bytes = sizeof(policy);
    auto error = RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &policy, &bytes);
    if (error == ERROR_FILE_NOT_FOUND) {
        policy = 1; // Windows policy: services only; never enable arbitrary UI apps.
        error = RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_DWORD, &policy, sizeof(policy));
    }
    if (error != ERROR_SUCCESS) {
        return static_cast<std::uint32_t>(error);
    }
    if (policy != 1 && policy != 3) {
        return ERROR_ACCESS_DISABLED_BY_POLICY;
    }
    struct Library {
        HMODULE value = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Library() {
            if (value) {
                FreeLibrary(value);
            }
        }
    } library;
    if (!library.value) {
        return GetLastError();
    }
    const auto send = reinterpret_cast<void(WINAPI*)(BOOL)>(GetProcAddress(library.value, "SendSAS"));
    if (!send) {
        return ERROR_PROC_NOT_FOUND;
    }
    HANDLE existing = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &existing)) {
        CloseHandle(existing);
        return ERROR_ACCESS_DENIED;
    }
    if (GetLastError() != ERROR_NO_TOKEN) {
        return GetLastError();
    }
    // The current console is serviced from Session 0. Keep the actual service
    // identity: a cloned SYSTEM token with only TokenSessionId changed is not
    // a logged-on client identity for SendSAS's impersonation path.
    send(FALSE);
    return ERROR_SUCCESS;
}
DesktopProcess::DesktopProcess(asio::any_io_executor executor, const DesktopIdentity& user,
                               const std::filesystem::path& executable, const std::wstring& pipeName,
                               DesktopWorkerRole workerRole) : state_(std::make_shared<State>(executor)) {
    const auto path = executable.native();
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\Ultraterimal-";
    if (!executable.is_absolute() || path.size() > 30000 || path.find_first_of(L"\"\r\n") != std::wstring::npos || path.find(L'\0') != std::wstring::npos ||
        !pipeName.starts_with(prefix) || pipeName.size() != prefix.size() + 32 ||
        !std::all_of(pipeName.begin() + prefix.size(), pipeName.end(), [](wchar_t value) { return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f'); })) {
        throw std::invalid_argument("Invalid desktop executable or pipe name");
    }
    if (user.state_->current) {
        requireActive(user.session());
    } else {
        requireConsole(user.session());
    }
    state_->job.reset(CreateJobObjectW(nullptr, nullptr));
    check(bool(state_->job), "Create desktop process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(state_->job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "Contain desktop process");
    struct Environment {
        void* value = nullptr;
        ~Environment() {
            if (value) {
                DestroyEnvironmentBlock(value);
            }
        }
    } environment;
    check(CreateEnvironmentBlock(&environment.value, user.state_->token.get(), FALSE), "Create desktop user environment");
    const auto role = workerRole == DesktopWorkerRole::kTool ? L" --desktop-tool --session " : L" --desktop --session ";
    auto command = L"\"" + path + L"\"" + role + std::to_wstring(user.session()) + L" --pipe " + pipeName + L" --parent " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\default";
    startup.lpDesktop = desktop;
    PROCESS_INFORMATION created{};
    const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    const auto directory = executable.parent_path().native();
    std::unique_ptr<SystemPrivileges> privileges;
    if (!user.state_->current) {
        privileges = std::make_unique<SystemPrivileges>();
    }
    const BOOL launched = user.state_->current
                              ? CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, flags, environment.value, directory.c_str(), &startup, &created)
                              : CreateProcessAsUserW(user.state_->token.get(), path.c_str(), command.data(), nullptr, nullptr, FALSE, flags, environment.value, directory.c_str(), &startup, &created);
    check(launched, "Launch desktop user process");
    UniqueHandle process(created.hProcess), thread(created.hThread);
    try {
        check(AssignProcessToJobObject(state_->job.get(), process.get()), "Assign desktop process job");
        check(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "Resume desktop process");
        state_->process = std::move(process);
        state_->id = created.dwProcessId;
    } catch (...) {
        TerminateProcess(process.get(), 1);
        throw;
    }
}
DesktopProcess::~DesktopProcess() { state_->job.reset(); }
std::uint32_t DesktopProcess::id() const { return state_->id; }
asio::awaitable<std::uint32_t> DesktopProcess::wait() { return State::waitOwned(state_); }
void DesktopProcess::terminate() { check(TerminateJobObject(state_->job.get(), 1), "Terminate desktop process tree"); }
} // namespace ultraterimal::agent
