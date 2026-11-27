#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "platform/platform.transport.h"
#include "ultraterimal/protocol/automation.protocol.h"

namespace ultraterimal::agent {
namespace {
std::string resolveExecShell() {
    for (const char* candidate : {"/bin/bash", "/usr/bin/bash", "/bin/sh"}) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return "/bin/sh";
}

void closeExtraFds(int keepA, int keepB) noexcept {
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd <= 0 || maxFd > 4096) {
        maxFd = 4096;
    }
    for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
        if (fd != keepA && fd != keepB) {
            ::close(fd);
        }
    }
}

void setNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void closeFd(int& fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
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

    if (!request.cwd().empty()) {
        struct stat st{};
        if (::stat(request.cwd().c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            result.set_success(false);
            result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
            result.set_exit_code(-1);
            return result;
        }
    }

    int stdoutPipe[2]{-1, -1};
    int stderrPipe[2]{-1, -1};
    if (::pipe(stdoutPipe) != 0) {
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }
    if (::pipe(stderrPipe) != 0) {
        closeFd(stdoutPipe[0]);
        closeFd(stdoutPipe[1]);
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }

    for (int fd : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]}) {
        (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }

    const std::string shell = resolveExecShell();
    const pid_t child = ::fork();
    if (child < 0) {
        for (int* fd : {&stdoutPipe[0], &stdoutPipe[1], &stderrPipe[0], &stderrPipe[1]}) {
            closeFd(*fd);
        }
        result.set_success(false);
        result.set_error(wire::EXEC_ERROR_SPAWN_FAILED);
        result.set_exit_code(-1);
        return result;
    }

    if (child == 0) {
        sigset_t empty{};
        sigemptyset(&empty);
        (void)::sigprocmask(SIG_SETMASK, &empty, nullptr);
        for (const int sig : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD}) {
            (void)::signal(sig, SIG_DFL);
        }
        ::setsid();
        const int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            if (devNull > STDERR_FILENO) {
                ::close(devNull);
            }
        }
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::dup2(stderrPipe[1], STDERR_FILENO);
        closeExtraFds(STDOUT_FILENO, STDERR_FILENO);
        if (!request.cwd().empty()) {
            if (::chdir(request.cwd().c_str()) != 0) {
                ::_exit(125);
            }
        } else if (const auto* pw = ::getpwuid(::geteuid()); pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
            (void)::chdir(pw->pw_dir);
        }
        char* const argv[] = {const_cast<char*>(shell.c_str()), const_cast<char*>("-c"), const_cast<char*>(request.command().c_str()), nullptr};
        ::execv(shell.c_str(), argv);
        ::_exit(127);
    }

    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[1]);
    setNonBlocking(stdoutPipe[0]);
    setNonBlocking(stderrPipe[0]);

    const std::size_t budget = request.max_output_bytes()
                                   ? std::min<std::size_t>(request.max_output_bytes(), protocol::kMaximumExecOutputBytes)
                                   : protocol::kDefaultExecOutputBytes;
    const std::uint32_t timeoutMs = request.timeout_ms()
                                        ? std::clamp(request.timeout_ms(), protocol::kMinimumExecTimeoutMs, protocol::kMaximumExecTimeoutMs)
                                        : protocol::kDefaultExecTimeoutMs;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    std::string stdoutData;
    std::string stderrData;
    bool truncated = false;
    bool timedOut = false;

    auto drainFd = [&](int& fd, std::string& target) {
        std::array<char, 4096> buffer{};
        while (fd >= 0) {
            const ssize_t n = ::read(fd, buffer.data(), buffer.size());
            if (n > 0) {
                const std::size_t total = stdoutData.size() + stderrData.size();
                if (total < budget) {
                    const std::size_t take = std::min(static_cast<std::size_t>(n), budget - total);
                    target.append(buffer.data(), take);
                    if (take < static_cast<std::size_t>(n)) {
                        truncated = true;
                    }
                } else {
                    truncated = true;
                }
                continue;
            }
            if (n == 0) {
                closeFd(fd);
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            closeFd(fd);
            break;
        }
    };

    while (stdoutPipe[0] >= 0 || stderrPipe[0] >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            timedOut = true;
            break;
        }
        const auto remainingMs = std::max<long long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        std::array<struct pollfd, 2> fds{};
        nfds_t count = 0;
        if (stdoutPipe[0] >= 0) {
            fds[count++] = {stdoutPipe[0], POLLIN, 0};
        }
        if (stderrPipe[0] >= 0) {
            fds[count++] = {stderrPipe[0], POLLIN, 0};
        }
        const int ready = ::poll(fds.data(), count, static_cast<int>(std::min<long long>(remainingMs, 1000)));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (stdoutPipe[0] >= 0) {
            drainFd(stdoutPipe[0], stdoutData);
        }
        if (stderrPipe[0] >= 0) {
            drainFd(stderrPipe[0], stderrData);
        }
    }

    if (timedOut) {
        (void)::kill(-child, SIGTERM);
        int status = 0;
        bool exited = false;
        for (int attempt = 0; attempt < 10; ++attempt) {
            const pid_t w = ::waitpid(child, &status, WNOHANG);
            if (w == child || (w < 0 && errno == ECHILD)) {
                exited = true;
                break;
            }
            ::usleep(20000);
        }
        if (!exited) {
            (void)::kill(-child, SIGKILL);
        }
    }

    closeFd(stdoutPipe[0]);
    closeFd(stderrPipe[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    int exitCode = -1;
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exitCode = 128 + WTERMSIG(status);
    }

    result.set_success(!timedOut);
    result.set_error(timedOut ? wire::EXEC_ERROR_TIMEOUT : wire::EXEC_ERROR_NONE);
    result.set_exit_code(exitCode);
    result.set_stdout_data(std::move(stdoutData));
    result.set_stderr_data(std::move(stderrData));
    result.set_truncated(truncated);
    result.set_timed_out(timedOut);
    return result;
}
} // namespace ultraterimal::agent
