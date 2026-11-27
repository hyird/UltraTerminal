#include <algorithm>
#include <array>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <pwd.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "platform/platform.transport.h"

namespace ultraterimal::agent {
namespace {
std::string resolveLoginShell() {
    if (const char* envShell = std::getenv("SHELL"); envShell && envShell[0] == '/' && ::access(envShell, X_OK) == 0) {
        return envShell;
    }
    if (const auto* pw = ::getpwuid(::geteuid()); pw && pw->pw_shell && pw->pw_shell[0] == '/' && ::access(pw->pw_shell, X_OK) == 0) {
        return pw->pw_shell;
    }
    for (const char* candidate : {"/bin/bash", "/usr/bin/bash", "/bin/sh"}) {
        if (::access(candidate, X_OK) == 0) {
            return candidate;
        }
    }
    return "/bin/sh";
}

void closeInheritedFds(int keepA, int keepB, int keepC) noexcept {
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd <= 0 || maxFd > 4096) {
        maxFd = 4096;
    }
    for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) {
        if (fd != keepA && fd != keepB && fd != keepC) {
            ::close(fd);
        }
    }
}

class LinuxTerminal final : public PlatformTerminal {
  public:
    LinuxTerminal(ruvia::event_loop loop, std::string shellPath, std::vector<std::string> shellArgs)
        : pty_(loop.executor()) {
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0) {
            throw std::system_error(errno, std::system_category(), "posix_openpt");
        }
        struct FdGuard {
            int fd;
            ~FdGuard() {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        } masterGuard{master};

        if (::grantpt(master) != 0 || ::unlockpt(master) != 0) {
            throw std::system_error(errno, std::system_category(), "grantpt/unlockpt");
        }
        char slaveName[128]{};
        if (::ptsname_r(master, slaveName, sizeof(slaveName)) != 0) {
            throw std::system_error(errno, std::system_category(), "ptsname_r");
        }

        struct winsize ws{25, 80, 0, 0};
        (void)::ioctl(master, TIOCSWINSZ, &ws);

        const pid_t child = ::fork();
        if (child < 0) {
            throw std::system_error(errno, std::system_category(), "fork pty child");
        }
        if (child == 0) {
            sigset_t empty{};
            sigemptyset(&empty);
            (void)::sigprocmask(SIG_SETMASK, &empty, nullptr);
            for (const int sig : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD}) {
                (void)::signal(sig, SIG_DFL);
            }
            ::setsid();
            const int slave = ::open(slaveName, O_RDWR);
            if (slave < 0) {
                ::_exit(126);
            }
            (void)::ioctl(slave, TIOCSCTTY, 0);
            ::dup2(slave, STDIN_FILENO);
            ::dup2(slave, STDOUT_FILENO);
            ::dup2(slave, STDERR_FILENO);
            if (slave > STDERR_FILENO) {
                ::close(slave);
            }
            closeInheritedFds(STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
            ::setenv("TERM", "xterm-256color", 1);
            if (const auto* pw = ::getpwuid(::geteuid()); pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
                (void)::chdir(pw->pw_dir);
                ::setenv("HOME", pw->pw_dir, 1);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(shellPath.c_str()));
            for (auto& arg : shellArgs) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);
            ::execv(shellPath.c_str(), argv.data());
            ::_exit(127);
        }

        pid_ = child;
        masterFd_ = master;
        masterGuard.fd = -1;
        const int flags = ::fcntl(masterFd_, F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK);
        }
        pty_.assign(masterFd_);
    }

    ~LinuxTerminal() override { close(); }

    asio::awaitable<std::string> read() override {
        std::array<char, 16384> buffer{};
        asio::error_code ec;
        const auto count = co_await pty_.async_read_some(asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, ec));
        if (ec || count == 0) {
            co_return std::string{};
        }
        co_return std::string(buffer.data(), count);
    }

    asio::awaitable<void> write(std::string_view bytes) override {
        if (bytes.empty()) {
            co_return;
        }
        std::string copy(bytes);
        co_await asio::async_write(pty_, asio::buffer(copy), asio::use_awaitable);
    }

    void resize(std::uint32_t columns, std::uint32_t rows) override {
        if (masterFd_ >= 0 && !closed_) {
            struct winsize ws{};
            ws.ws_col = static_cast<unsigned short>(std::clamp<std::uint32_t>(columns, 1, 1000));
            ws.ws_row = static_cast<unsigned short>(std::clamp<std::uint32_t>(rows, 1, 1000));
            (void)::ioctl(masterFd_, TIOCSWINSZ, &ws);
        }
    }

    std::uint32_t waitExit() override {
        if (pid_ <= 0) {
            return exitCode_;
        }
        int status = 0;
        for (int attempt = 0; attempt < 20; ++attempt) {
            const pid_t res = ::waitpid(pid_, &status, WNOHANG);
            if (res == pid_) {
                pid_ = -1;
                exitCode_ = WIFEXITED(status) ? static_cast<std::uint32_t>(WEXITSTATUS(status))
                                              : (WIFSIGNALED(status) ? static_cast<std::uint32_t>(128 + WTERMSIG(status)) : 1u);
                return exitCode_;
            }
            if (res < 0 && errno != EINTR) {
                pid_ = -1;
                return exitCode_;
            }
            ::usleep(25000);
        }
        (void)::kill(-pid_, SIGKILL);
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
        exitCode_ = WIFEXITED(status) ? static_cast<std::uint32_t>(WEXITSTATUS(status)) : 1u;
        return exitCode_;
    }

    void close() noexcept override {
        if (closed_) {
            return;
        }
        closed_ = true;
        asio::error_code ignored;
        pty_.cancel(ignored);
        pty_.close(ignored);
        masterFd_ = -1;
        if (pid_ > 0) {
            (void)::kill(-pid_, SIGHUP);
            int status = 0;
            bool reaped = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
                const pid_t res = ::waitpid(pid_, &status, WNOHANG);
                if (res == pid_ || (res < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                ::usleep(10000);
            }
            if (!reaped) {
                (void)::kill(-pid_, SIGKILL);
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
                }
            }
            pid_ = -1;
        }
    }

  private:
    asio::posix::stream_descriptor pty_;
    int masterFd_ = -1;
    pid_t pid_ = -1;
    std::uint32_t exitCode_ = 0;
    bool closed_ = false;
};
} // namespace

std::unique_ptr<PlatformTerminal> PlatformTerminal::spawn(ruvia::event_loop loop) {
    return std::make_unique<LinuxTerminal>(std::move(loop), resolveLoginShell(), std::vector<std::string>{"-i"});
}

TerminalDiagnostic checkLocalTerminal() {
    ruvia::blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 4});
    ruvia::event_loop_pool loopPool({.loop_count_ = 1, .queue_capacity_ = 16, .blocking_pool_ = &pool});
    auto loop = loopPool.next_loop();
    LinuxTerminal terminal(loop, "/bin/sh", {"-c", "echo ULTRATERIMAL_CONPTY_OK"});
    terminal.resize(100, 30);
    std::string output;
    loopPool.start();
    std::promise<void> done;
    auto doneFuture = done.get_future();
    asio::co_spawn(
        loop.executor(),
        [&]() -> asio::awaitable<void> {
            for (;;) {
                auto chunk = co_await terminal.read();
                if (chunk.empty()) {
                    break;
                }
                if (output.size() + chunk.size() <= 65536) {
                    output += chunk;
                }
            }
        },
        [&](std::exception_ptr) { done.set_value(); });
    doneFuture.wait();
    const std::uint32_t exitCode = terminal.waitExit();
    terminal.close();
    loopPool.request_stop();
    loopPool.join();
    pool.join();
    return {exitCode, output.find("ULTRATERIMAL_CONPTY_OK") != std::string::npos};
}
} // namespace ultraterimal::agent
