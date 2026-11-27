#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
constexpr std::uint64_t kMaximumLifetimeSeconds = 300;
std::mutex gRestartMutex;

bool validOperationId(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char digit : value) {
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool validDeviceId(std::string_view value) noexcept {
    return value.size() == 66 && value.starts_with("d-") && validOperationId(value.substr(2));
}

wire::WindowsRestartResult resultFor(const wire::WindowsRestartRequest& request, wire::WindowsRestartResult::Status status, std::uint32_t error = 0) {
    wire::WindowsRestartResult result;
    result.set_operation_id(request.operation_id());
    result.set_status(status);
    result.set_win32_error(error);
    return result;
}

std::uint64_t currentUnixSeconds() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

class LinuxSystemRestartPlatform final : public SystemRestartPlatform {
  public:
    bool isEligible() noexcept override { return ::geteuid() == 0; }

    bool persistOperation(std::string_view operationId) override {
        if (!validOperationId(operationId)) {
            throw std::system_error(EINVAL, std::system_category(), "Invalid restart operation");
        }
        std::scoped_lock lock(gRestartMutex);
        const std::filesystem::path dir("/var/lib/ultraterimal/restarts");
        std::filesystem::create_directories(dir);
        ::chmod("/var/lib/ultraterimal", 0700);
        ::chmod(dir.c_str(), 0700);
        const auto target = dir / std::string(operationId);
        const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                return false;
            }
            throw std::system_error(errno, std::system_category(), "Persist restart operation");
        }
        constexpr char marker[] = "UTRR1";
        (void)::write(fd, marker, sizeof(marker));
        (void)::fsync(fd);
        ::close(fd);
        return true;
    }

    std::uint32_t initiatePlannedRestart() noexcept override {
        const pid_t pid = ::fork();
        if (pid < 0) {
            return static_cast<std::uint32_t>(errno);
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
            // Brief pause lets the gateway receive the ACCEPTED acknowledgment before systemd shuts down.
            ::usleep(300000);
            const char* systemctl = ::access("/usr/bin/systemctl", X_OK) == 0 ? "/usr/bin/systemctl" : "/bin/systemctl";
            char* const argv[] = {const_cast<char*>(systemctl), const_cast<char*>("reboot"), const_cast<char*>("--no-block"), const_cast<char*>("--no-wall"), nullptr};
            ::execv(systemctl, argv);
            ::sync();
            ::reboot(RB_AUTOBOOT);
            ::_exit(127);
        }
        return 0;
    }
};
} // namespace

wire::WindowsRestartResult handleSystemRestart(std::string_view deviceId, const wire::WindowsRestartRequest& request,
                                               SystemRestartPlatform& platform, std::uint64_t nowUnixSeconds) {
    if (!validOperationId(request.operation_id()) || !validDeviceId(deviceId) || request.device_id() != deviceId ||
        request.expires_at_unix_seconds() <= nowUnixSeconds || request.expires_at_unix_seconds() - nowUnixSeconds > kMaximumLifetimeSeconds) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, EINVAL);
    }
    if (!platform.isEligible()) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, EACCES);
    }
    try {
        if (!platform.persistOperation(request.operation_id())) {
            return resultFor(request, wire::WindowsRestartResult::UNKNOWN, EEXIST);
        }
    } catch (const std::system_error& error) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, static_cast<std::uint32_t>(error.code().value()));
    } catch (...) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, EIO);
    }
    if (request.expires_at_unix_seconds() <= currentUnixSeconds()) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, ETIMEDOUT);
    }
    const auto error = platform.initiatePlannedRestart();
    if (error != 0) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, error);
    }
    return resultFor(request, wire::WindowsRestartResult::ACCEPTED);
}

SystemRestartPlatform& nativeSystemRestartPlatform() {
    static LinuxSystemRestartPlatform platform;
    return platform;
}

bool canManageSystemPower() noexcept {
    return nativeSystemRestartPlatform().isEligible();
}

std::uint64_t systemBootTimeUnixSeconds() noexcept {
    static const std::uint64_t bootTime = []() -> std::uint64_t {
        std::ifstream stat("/proc/stat");
        std::string line;
        while (std::getline(stat, line)) {
            if (line.starts_with("btime ")) {
                std::istringstream iss(line.substr(6));
                std::uint64_t value = 0;
                if (iss >> value) {
                    return value;
                }
            }
        }
        return 0;
    }();
    return bootTime;
}
} // namespace ultraterimal::agent
