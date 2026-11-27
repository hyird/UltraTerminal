#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "config/version.config.h"
#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
void writeResult(const std::filesystem::path& root, std::uint32_t code) {
    const auto temp = root / ".update/result.tmp";
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output << code;
    output.close();
    if (!output || ::rename(temp.c_str(), (root / ".update/result").c_str()) != 0) {
        throw std::runtime_error("Cannot persist update result");
    }
}
} // namespace

bool launchAgentUpdate(const std::filesystem::path& executable, const wire::AgentUpdate& update) {
    if (::geteuid() != 0 || !protocol::newerUpdate(update, kAgentVersion)) {
        return false;
    }
    const auto root = executable.parent_path();
    const auto dir = root / ".update";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    (void)::chmod(dir.c_str(), 0700);
    const auto helper = dir / "updater";
    std::filesystem::copy_file(executable, helper, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    (void)::chmod(helper.c_str(), 0755);
    std::ofstream request(dir / "request", std::ios::binary | std::ios::trunc);
    if (!update.SerializeToOstream(&request)) {
        return false;
    }
    request.close();
    if (!request) {
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        ::setsid();
        const int nullFd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullFd >= 0) {
            ::dup2(nullFd, STDIN_FILENO);
            ::dup2(nullFd, STDOUT_FILENO);
            ::dup2(nullFd, STDERR_FILENO);
            if (nullFd > STDERR_FILENO) {
                ::close(nullFd);
            }
        }
        const std::string helperStr = helper.string();
        char* const argv[] = {const_cast<char*>(helperStr.c_str()), const_cast<char*>("--apply-update"), nullptr};
        ::execv(helperStr.c_str(), argv);
        ::_exit(127);
    }
    return true;
}

struct PlatformAgentUpdate::State {
    std::filesystem::path directory = agentExecutablePath().parent_path();
    int lockFd = -1;
    wire::AgentUpdate request;

    State() {
        if (::geteuid() != 0 || directory.filename() != ".update") {
            throw std::runtime_error("Update process unavailable");
        }
        const auto lockPath = directory / "lock";
        lockFd = ::open(lockPath.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) {
                ::close(lockFd);
            }
            throw std::runtime_error("Another update is already running");
        }
        std::ifstream input(directory / "request", std::ios::binary);
        std::string bytes(1025, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (bytes.size() > 1024 || !request.ParseFromString(bytes) || !protocol::newerUpdate(request, kAgentVersion)) {
            ::close(lockFd);
            throw std::runtime_error("Invalid update request");
        }
    }
    ~State() {
        if (lockFd >= 0) {
            ::flock(lockFd, LOCK_UN);
            ::close(lockFd);
        }
    }
};

PlatformAgentUpdate::PlatformAgentUpdate() : state_(std::make_unique<State>()) {}
PlatformAgentUpdate::~PlatformAgentUpdate() = default;
std::filesystem::path PlatformAgentUpdate::root() const { return state_->directory.parent_path(); }
std::filesystem::path PlatformAgentUpdate::configPath() const {
    const std::filesystem::path systemConfig("/etc/ultraterimal/agent.json");
    std::error_code ignored;
    if (std::filesystem::exists(systemConfig, ignored)) {
        return systemConfig;
    }
    return root() / "agent.json";
}
std::filesystem::path PlatformAgentUpdate::stagePath() const { return state_->directory / "agent.new"; }
const wire::AgentUpdate& PlatformAgentUpdate::request() const { return state_->request; }

void PlatformAgentUpdate::failed(std::uint32_t code) noexcept {
    try {
        writeResult(root(), code);
    } catch (...) {
    }
}

int PlatformAgentUpdate::install() {
    const auto installed = root();
    const auto staged = stagePath();
    const auto target = installed / "agent";
    try {
        if (::chmod(staged.c_str(), 0755) != 0) {
            throw std::system_error(errno, std::system_category(), "chmod staged agent");
        }
        std::error_code ec;
        std::filesystem::copy_file(target, state_->directory / "agent.bak", std::filesystem::copy_options::overwrite_existing, ec);
        if (::rename(staged.c_str(), target.c_str()) != 0) {
            throw std::system_error(errno, std::system_category(), "replace agent binary");
        }
        writeResult(installed, 0);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            ::usleep(200000);
            const char* systemctl = ::access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl";
            char* const argv[] = {const_cast<char*>(systemctl), const_cast<char*>("restart"), const_cast<char*>("ultraterimal-agent.service"), nullptr};
            ::execv(systemctl, argv);
            ::_exit(127);
        }
        return 0;
    } catch (...) {
        failed(1);
        return 1;
    }
}

std::optional<std::uint32_t> takeAgentUpdateResult(const std::filesystem::path& root) {
    const auto file = root / ".update/result";
    std::ifstream input(file);
    std::uint32_t result = 0;
    if (!(input >> result)) {
        return {};
    }
    input.close();
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    return result;
}
} // namespace ultraterimal::agent
