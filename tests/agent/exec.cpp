#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include "platform/platform.transport.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    using namespace ultraterimal;
    try {
        {
            wire::ExecCommandRequest req;
            req.set_request_id(1);
#ifdef _WIN32
            req.set_command("echo OUT_OK & echo ERR_OK 1>&2");
#else
            req.set_command("echo OUT_OK; echo ERR_OK >&2");
#endif
            const auto res = agent::runPlatformExec(req);
            require(res.success() && res.exit_code() == 0, "Basic exec failed");
            require(res.stdout_data().find("OUT_OK") != std::string::npos, "Missing stdout");
            require(res.stderr_data().find("ERR_OK") != std::string::npos, "Missing stderr");
        }
        {
            wire::ExecCommandRequest req;
            req.set_request_id(2);
#ifdef _WIN32
            req.set_command("exit /b 42");
#else
            req.set_command("exit 42");
#endif
            const auto res = agent::runPlatformExec(req);
            require(res.success() && res.exit_code() == 42, "Non-zero exit code mismatch");
        }
        {
            wire::ExecCommandRequest req;
            req.set_request_id(3);
            const auto tempDir = std::filesystem::temp_directory_path();
            req.set_cwd(tempDir.string());
#ifdef _WIN32
            req.set_command("cd");
#else
            req.set_command("pwd");
#endif
            const auto res = agent::runPlatformExec(req);
            require(res.success() && res.exit_code() == 0 && !res.stdout_data().empty(), "Working directory exec failed");
        }
        {
            wire::ExecCommandRequest req;
            req.set_request_id(4);
            req.set_timeout_ms(200);
#ifdef _WIN32
            req.set_command("ping -n 6 127.0.0.1 >nul");
#else
            req.set_command("sleep 3");
#endif
            const auto res = agent::runPlatformExec(req);
            require(!res.success() && res.timed_out() && res.error() == wire::EXEC_ERROR_TIMEOUT, "Timeout enforcement failed");
        }
        {
            wire::ExecCommandRequest req;
            req.set_request_id(5);
            req.set_max_output_bytes(64);
#ifdef _WIN32
            req.set_command("for /L %i in (1,1,50) do @echo 0123456789ABCDEF");
#else
            req.set_command("yes 0123456789ABCDEF | head -n 50");
#endif
            const auto res = agent::runPlatformExec(req);
            require(res.success() && res.truncated() && res.stdout_data().size() + res.stderr_data().size() <= 64, "Output truncation failed");
        }

        std::cout << "Platform exec command checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
