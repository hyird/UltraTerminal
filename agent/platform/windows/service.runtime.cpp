#include "platform/windows/service.runtime.h"

#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "console_session.transport.h"
#include "login_install.transport.h"

namespace ultraterimal::agent {
namespace {
constexpr wchar_t kServiceName[] = L"UltraterimalAgent";
class ServiceHandle {
  public:
    explicit ServiceHandle(SC_HANDLE handle) : handle_(handle) {
        if (!handle_) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Service Control Manager");
        }
    }
    ~ServiceHandle() { CloseServiceHandle(handle_); }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;
    SC_HANDLE get() const { return handle_; }

  private:
    SC_HANDLE handle_;
};
void check(BOOL ok, const char* operation) {
    if (!ok) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}
SERVICE_STATUS_PROCESS query(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    check(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes), "QueryServiceStatusEx");
    return status;
}
void awaitState(SC_HANDLE service, DWORD target) {
    const auto deadline = GetTickCount64() + 30000;
    for (;;) {
        const auto status = query(service);
        if (status.dwCurrentState == target) {
            return;
        }
        if (target == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            throw std::system_error(static_cast<int>(status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE), std::system_category(), "Service stopped during startup");
        }
        if (GetTickCount64() >= deadline) {
            throw std::runtime_error("Service transition timed out after 30 seconds");
        }
        Sleep(100);
    }
}
} // namespace
std::filesystem::path agentExecutablePath() {
    std::vector<wchar_t> path(32768);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length) {
        check(FALSE, "GetModuleFileNameW");
    }
    if (length >= path.size()) {
        throw std::runtime_error("Executable path is too long");
    }
    return std::wstring(path.data(), length);
}
namespace {
struct ServiceRuntime {
    ServiceTask task;
    ConsoleSessionEvents consoleEvents;
    std::stop_source stopSource;
    SERVICE_STATUS_HANDLE statusHandle = nullptr;
    SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 1, 5000};
    std::mutex mutex;
    DWORD failure = NO_ERROR;
    void publish(DWORD state, DWORD error = NO_ERROR) {
        status.dwCurrentState = state;
        status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
        status.dwWin32ExitCode = error;
        status.dwCheckPoint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 1 : 0;
        status.dwWaitHint = status.dwCheckPoint ? 5000 : 0;
        if (!SetServiceStatus(statusHandle, &status)) {
            failure = GetLastError();
        }
        if (error != NO_ERROR) {
            failure = error;
        }
    }
};
// The SCM owns callback sequencing; context remains alive through dispatcher exit.
ServiceRuntime* activeRuntime = nullptr;
DWORD WINAPI control(DWORD command, DWORD, void*, void* context) noexcept {
    auto& runtime = *static_cast<ServiceRuntime*>(context);
    try {
        std::lock_guard lock(runtime.mutex);
        if (command == SERVICE_CONTROL_STOP || command == SERVICE_CONTROL_SHUTDOWN) {
            if (runtime.status.dwCurrentState == SERVICE_RUNNING) {
                runtime.publish(SERVICE_STOP_PENDING);
                runtime.stopSource.request_stop();
            }
            return NO_ERROR;
        }
        if (command == SERVICE_CONTROL_SESSIONCHANGE) {
            runtime.consoleEvents.notify();
            return NO_ERROR;
        }
        if (command == SERVICE_CONTROL_INTERROGATE) {
            return NO_ERROR;
        }
        return ERROR_CALL_NOT_IMPLEMENTED;
    } catch (...) {
        return ERROR_EXCEPTION_IN_SERVICE;
    }
}
void WINAPI serviceMain(DWORD, wchar_t**) noexcept {
    auto& runtime = *activeRuntime;
    runtime.statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, control, &runtime);
    if (!runtime.statusHandle) {
        runtime.failure = GetLastError();
        return;
    }
    try {
        {
            std::lock_guard lock(runtime.mutex);
            runtime.publish(SERVICE_START_PENDING);
            if (runtime.failure != NO_ERROR) {
                runtime.publish(SERVICE_STOPPED, runtime.failure);
                return;
            }
            runtime.publish(SERVICE_RUNNING);
            if (runtime.failure != NO_ERROR) {
                runtime.publish(SERVICE_STOPPED, runtime.failure);
                return;
            }
        }
        runtime.task(runtime.stopSource.get_token(), runtime.consoleEvents);
        std::lock_guard lock(runtime.mutex);
        runtime.publish(SERVICE_STOPPED);
    } catch (...) {
        std::lock_guard lock(runtime.mutex);
        runtime.publish(SERVICE_STOPPED, ERROR_EXCEPTION_IN_SERVICE);
    }
}
} // namespace
std::filesystem::path serviceConfigPath() {
    return agentExecutablePath().parent_path() / L"agent.json";
}
void dispatchService(ServiceTask task) {
    if (!task) {
        throw std::invalid_argument("A service task is required");
    }
    ServiceRuntime runtime;
    runtime.task = std::move(task);
    activeRuntime = &runtime;
    SERVICE_TABLE_ENTRYW table[]{{const_cast<wchar_t*>(kServiceName), serviceMain}, {nullptr, nullptr}};
    const auto ok = StartServiceCtrlDispatcherW(table);
    const auto error = GetLastError();
    activeRuntime = nullptr;
    if (!ok) {
        throw std::system_error(static_cast<int>(error), std::system_category(), "StartServiceCtrlDispatcherW (use --start from a console)");
    }
    if (runtime.failure != NO_ERROR) {
        throw std::system_error(static_cast<int>(runtime.failure), std::system_category(), "Service runtime");
    }
}
void installService() {
    const auto command = L"\"" + agentExecutablePath().native() + L"\" --service";
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    // Register only; never overwrite an existing service or silently start it.
    ServiceHandle service(CreateServiceW(manager.get(), kServiceName, L"Ultraterimal Agent", SERVICE_CHANGE_CONFIG | DELETE,
                                         SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                                         L"LocalSystem", nullptr));
    SERVICE_DESCRIPTIONW description{const_cast<wchar_t*>(L"Ultraterimal remote desktop Agent and authorized Windows login service.")};
    if (!ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description)) {
        const auto error = GetLastError();
        DeleteService(service.get());
        throw std::system_error(static_cast<int>(error), std::system_category(), "ChangeServiceConfig2W");
    }
    try {
        installWindowsLoginProvider(agentExecutablePath().parent_path() / L"ultraterimal_login.dll");
    } catch (...) {
        DeleteService(service.get());
        throw;
    }
}
void uninstallService() {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ServiceHandle service(OpenServiceW(manager.get(), kServiceName, DELETE | SERVICE_QUERY_STATUS));
    if (query(service.get()).dwCurrentState != SERVICE_STOPPED) {
        throw std::runtime_error("Stop the service before uninstalling it");
    }
    removeWindowsLoginProvider(agentExecutablePath().parent_path() / L"ultraterimal_login.dll");
    check(DeleteService(service.get()), "DeleteService");
}
void startService() {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ServiceHandle service(OpenServiceW(manager.get(), kServiceName, SERVICE_START | SERVICE_QUERY_STATUS));
    if (query(service.get()).dwCurrentState == SERVICE_RUNNING) {
        return;
    }
    if (!StartServiceW(service.get(), 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        check(FALSE, "StartServiceW");
    }
    awaitState(service.get(), SERVICE_RUNNING);
}
void stopService() {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    ServiceHandle service(OpenServiceW(manager.get(), kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS));
    const auto current = query(service.get());
    if (current.dwCurrentState == SERVICE_STOPPED) {
        return;
    }
    if (current.dwCurrentState != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status{};
        check(ControlService(service.get(), SERVICE_CONTROL_STOP, &status), "ControlService");
    }
    awaitState(service.get(), SERVICE_STOPPED);
}
std::string serviceStatus() {
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    auto rawService = OpenServiceW(manager.get(), kServiceName, SERVICE_QUERY_STATUS);
    if (!rawService && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        return "not_installed";
    }
    ServiceHandle service(rawService);
    switch (query(service.get()).dwCurrentState) {
    case SERVICE_STOPPED:
        return "stopped";
    case SERVICE_START_PENDING:
        return "starting";
    case SERVICE_STOP_PENDING:
        return "stopping";
    case SERVICE_RUNNING:
        return "running";
    default:
        return "other";
    }
}
} // namespace ultraterimal::agent
