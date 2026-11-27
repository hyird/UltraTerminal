#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
constexpr const char* kServiceUnit = "/etc/systemd/system/ultraterimal-agent.service";
constexpr const char* kServiceName = "ultraterimal-agent.service";

const char* systemctlPath() {
    if (::access("/usr/bin/systemctl", X_OK) == 0) {
        return "/usr/bin/systemctl";
    }
    if (::access("/bin/systemctl", X_OK) == 0) {
        return "/bin/systemctl";
    }
    throw std::runtime_error("systemctl is unavailable");
}

int runSystemctl(std::initializer_list<const char*> args, std::string* capturedStdout = nullptr) {
    int pipeFds[2]{-1, -1};
    if (capturedStdout && ::pipe2(pipeFds, O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::system_category(), "pipe2");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (capturedStdout) {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
        }
        throw std::system_error(errno, std::system_category(), "fork systemctl");
    }
    if (pid == 0) {
        if (capturedStdout) {
            ::close(pipeFds[0]);
            ::dup2(pipeFds[1], STDOUT_FILENO);
            ::close(pipeFds[1]);
        }
        std::vector<char*> argv;
        const char* exe = systemctlPath();
        argv.push_back(const_cast<char*>(exe));
        for (const char* arg : args) {
            argv.push_back(const_cast<char*>(arg));
        }
        argv.push_back(nullptr);
        ::execv(exe, argv.data());
        ::_exit(127);
    }
    if (capturedStdout) {
        ::close(pipeFds[1]);
        char buf[256];
        for (;;) {
            const ssize_t n = ::read(pipeFds[0], buf, sizeof(buf));
            if (n > 0) {
                capturedStdout->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
        ::close(pipeFds[0]);
        while (!capturedStdout->empty() && (capturedStdout->back() == '\n' || capturedStdout->back() == '\r')) {
            capturedStdout->pop_back();
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::system_category(), "waitpid systemctl");
        }
    }
    if (!WIFEXITED(status)) {
        return 128;
    }
    return WEXITSTATUS(status);
}

void notifySystemd(std::string_view message) noexcept {
    const char* socketPath = std::getenv("NOTIFY_SOCKET");
    if (!socketPath || socketPath[0] == '\0') {
        return;
    }
    const std::size_t pathLen = std::strlen(socketPath);
    if (pathLen >= sizeof(sockaddr_un::sun_path)) {
        return;
    }
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath, pathLen);
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';
    }
    const socklen_t addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + pathLen);
    (void)::sendto(fd, message.data(), message.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr), addrLen);
    ::close(fd);
}
} // namespace

std::filesystem::path agentExecutablePath() {
    return std::filesystem::canonical("/proc/self/exe");
}

std::filesystem::path serviceConfigPath() {
    const std::filesystem::path systemConfig("/etc/ultraterimal/agent.json");
    std::error_code ignored;
    if (std::filesystem::exists(systemConfig, ignored)) {
        return systemConfig;
    }
    return agentExecutablePath().parent_path() / "agent.json";
}

void dispatchService(ServiceTask task) {
    if (::isatty(STDIN_FILENO) != 0 && !std::getenv("NOTIFY_SOCKET") && !std::getenv("INVOCATION_ID")) {
        throw std::runtime_error("Use --connect for interactive execution; --service runs under systemd");
    }
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        throw std::runtime_error("Cannot block service termination signals");
    }

    std::stop_source stopSource;
    std::jthread signalWatcher([&](std::stop_token watcherStop) {
        while (!watcherStop.stop_requested()) {
            timespec timeout{0, 250000000L};
            const int sig = ::sigtimedwait(&mask, nullptr, &timeout);
            if (sig == SIGTERM || sig == SIGINT) {
                stopSource.request_stop();
                break;
            }
        }
    });

    notifySystemd("READY=1\n");
    ConsoleSessionEvents events;
    try {
        task(stopSource.get_token(), events);
    } catch (...) {
        notifySystemd("STOPPING=1\n");
        signalWatcher.request_stop();
        throw;
    }
    notifySystemd("STOPPING=1\n");
    signalWatcher.request_stop();
}

void installService() {
    if (::geteuid() != 0) {
        throw std::runtime_error("Administrator (root) privileges are required");
    }
    const auto exe = agentExecutablePath();
    std::ofstream unit(kServiceUnit, std::ios::binary | std::ios::trunc);
    if (!unit) {
        throw std::runtime_error("Cannot write systemd unit file");
    }
    unit << "[Unit]\n"
            "Description=Ultraterimal Agent\n"
            "After=network-online.target\n"
            "Wants=network-online.target\n\n"
            "[Service]\n"
            "Type=notify\n"
            "ExecStart="
         << exe.string() << " --service\n"
                            "Restart=always\n"
                            "RestartSec=3\n"
                            "LimitNOFILE=65536\n\n"
                            "[Install]\n"
                            "WantedBy=multi-user.target\n";
    unit.close();
    if (!unit) {
        throw std::runtime_error("Cannot flush systemd unit file");
    }
    (void)::chmod(kServiceUnit, 0644);
    if (runSystemctl({"daemon-reload"}) != 0 || runSystemctl({"enable", kServiceName}) != 0) {
        throw std::runtime_error("Cannot enable systemd service");
    }
}

void uninstallService() {
    if (::geteuid() != 0) {
        throw std::runtime_error("Administrator (root) privileges are required");
    }
    (void)runSystemctl({"stop", kServiceName});
    (void)runSystemctl({"disable", kServiceName});
    std::error_code ignored;
    std::filesystem::remove(kServiceUnit, ignored);
    (void)runSystemctl({"daemon-reload"});
}

void startService() {
    if (::geteuid() != 0) {
        throw std::runtime_error("Administrator (root) privileges are required");
    }
    if (runSystemctl({"start", kServiceName}) != 0) {
        throw std::runtime_error("Cannot start systemd service");
    }
}

void stopService() {
    if (::geteuid() != 0) {
        throw std::runtime_error("Administrator (root) privileges are required");
    }
    if (runSystemctl({"stop", kServiceName}) != 0) {
        throw std::runtime_error("Cannot stop systemd service");
    }
}

std::string serviceStatus() {
    std::error_code ignored;
    if (!std::filesystem::exists(kServiceUnit, ignored)) {
        return "not_installed";
    }
    std::string out;
    const int code = runSystemctl({"is-active", kServiceName}, &out);
    if (code == 0 && out == "active") {
        return "running";
    }
    return out.empty() ? "stopped" : out;
}
} // namespace ultraterimal::agent
