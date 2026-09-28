#include "update.transport.h"

#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <windows.h>

#include <msi.h>

#include "config/version.config.h"
#include "service.runtime.h"
#include "system_restart.transport.h"

namespace ultraterimal::agent {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value) {
            CloseHandle(value);
        }
    }
};
void writeResult(const std::filesystem::path& root, std::uint32_t code) {
    const auto temporary = root / ".update/result.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << code;
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), (root / ".update/result").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot retain update result");
    }
}
void automaticService() {
    const auto manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        throw std::runtime_error("Cannot open service manager");
    }
    const auto service = OpenServiceW(manager, L"UltraterimalAgent", SERVICE_CHANGE_CONFIG);
    const auto changed = service && ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (service) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    if (!changed) {
        throw std::runtime_error("Cannot configure Agent service");
    }
}
} // namespace
bool launchAgentUpdate(const std::filesystem::path& executable, const wire::AgentUpdate& update) {
    if (!nativeSystemRestartPlatform().isSessionZeroLocalSystem() || !protocol::newerUpdate(update, kAgentVersion)) {
        return false;
    }
    const auto root = executable.parent_path();
    const auto directory = root / ".update";
    std::filesystem::create_directory(directory);
    if ((GetFileAttributesW(directory.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    const auto helper = directory / "updater.exe";
    // A running updater locks its image. Refuse concurrent replacement.
    std::filesystem::copy_file(executable, helper, std::filesystem::copy_options::overwrite_existing);
    std::ofstream request(directory / "request", std::ios::binary | std::ios::trunc);
    if (!update.SerializeToOstream(&request)) {
        return false;
    }
    request.close();
    if (!request) {
        return false;
    }
    std::wstring command = L"\"" + helper.native() + L"\" --apply-update";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, root.c_str(), &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
struct WindowsAgentUpdate::State {
    Handle mutex{CreateMutexW(nullptr, TRUE, L"Global\\UltraterimalAgentUpdate")};
    bool duplicate = GetLastError() == ERROR_ALREADY_EXISTS;
    std::filesystem::path directory = agentExecutablePath().parent_path();
    wire::AgentUpdate request;
    State() {
        if (!mutex.value || duplicate ||
            !nativeSystemRestartPlatform().isSessionZeroLocalSystem() || directory.filename() != L".update") {
            throw std::runtime_error("Update process unavailable");
        }
        std::ifstream input(directory / "request", std::ios::binary);
        std::string bytes(1025, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (bytes.size() > 1024 || !request.ParseFromString(bytes) || !protocol::newerUpdate(request, kAgentVersion)) {
            throw std::runtime_error("Invalid update request");
        }
    }
};
WindowsAgentUpdate::WindowsAgentUpdate() : state_(std::make_unique<State>()) {}
WindowsAgentUpdate::~WindowsAgentUpdate() = default;
std::filesystem::path WindowsAgentUpdate::root() const { return state_->directory.parent_path(); }
const wire::AgentUpdate& WindowsAgentUpdate::request() const { return state_->request; }
void WindowsAgentUpdate::failed(std::uint32_t code) noexcept {
    try {
        writeResult(root(), code);
    } catch (...) {
    }
}
int WindowsAgentUpdate::install() {
    const auto directory = state_->directory;
    const auto installed = root();
    std::uint32_t result = ERROR_INSTALL_FAILURE;
    bool installAttempted = false;
    try {
        std::filesystem::create_directory(directory / "backup");
        for (const auto* file : {L"agent.exe", L"ultraterimal_login.dll", L"agent.json", L"agent.credential", L"ca.pem"}) {
            std::filesystem::copy_file(installed / file, directory / "backup" / file, std::filesystem::copy_options::overwrite_existing);
        }
        installAttempted = true;
        MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
        result = MsiInstallProductW((directory / "package.msi").c_str(), L"REBOOT=ReallySuppress");
        writeResult(installed, result);
        automaticService();
        startService();
    } catch (...) {
        failed(result == ERROR_SUCCESS ? ERROR_SERVICE_NOT_ACTIVE : result);
        if (installAttempted) {
            try {
                automaticService();
                startService();
            } catch (...) {
            }
        }
        return 1;
    }
    return result == ERROR_SUCCESS || result == ERROR_SUCCESS_REBOOT_REQUIRED ? 0 : 1;
}
std::optional<std::uint32_t> takeAgentUpdateResult(const std::filesystem::path& root) {
    const auto file = root / ".update/result";
    std::ifstream input(file);
    std::uint32_t result = 0;
    if (!(input >> result)) {
        return {};
    }
    input.close();
    std::filesystem::remove(file);
    return result;
}
} // namespace ultraterimal::agent
