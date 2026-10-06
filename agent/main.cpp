#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_future.hpp>
#include <csignal>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/execution_domain.h>

#include "config/command_line.config.h"
#include "config/version.config.h"
#include "features/connection/control.runtime.h"
#include "features/desktop/worker.runtime.h"
#include "features/enrollment/enrollment.runtime.h"
#include "features/update/update.runtime.h"
#include "platform/windows/capability.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/login_install.transport.h"
#include "platform/windows/pipe.transport.h"
#include "platform/windows/service.runtime.h"
#include "platform/windows/terminal.transport.h"

int wmain(int argc, wchar_t** argv) {
    using namespace ultraterimal::agent;
    bool background = false;
    try {
        std::vector<std::wstring_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        const auto options = parseCommandLine(arguments);
        background = options && (options->command == Command::kService || options->command == Command::kDesktop || options->command == Command::kApplyUpdate);
        if (!options) {
            std::wcerr << options.error() << L'\n';
            return 2;
        }
        switch (options->command) {
        case Command::kApplyUpdate:
            return runAgentUpdate();
        case Command::kHelp:
            std::cout << "Ultraterimal Agent\n"
                         "  --terminal-check  Verify local ConPTY without remote access\n"
                         "  --connection-check <config.json>  Verify authenticated native UDP/TCP control handshake\n"
                         "  --connect <config.json>  Maintain the control connection until Ctrl+C\n"
                         "  --enroll <bootstrap.json>  Read code/name JSON from stdin; emit connection config\n"
                         "  --probe      Report local API/display capabilities as JSON\n"
                         "  --status     Query Windows service state\n"
                         "  --install    Register auto-start service (administrator)\n"
                         "  --start      Start installed service (administrator)\n"
                         "  --stop       Stop installed service (administrator)\n"
                         "  --uninstall  Remove stopped service (administrator)\n"
                         "  --install-login-provider  Register bundled Windows login provider (administrator)\n"
                         "  --remove-login-provider   Unregister bundled Windows login provider (administrator)\n"
                         "  --service    SCM entry point; not an interactive command\n"
                         "  --desktop --session <id> --pipe <name> --parent <pid>  Internal desktop worker\n"
                         "  --version    Print version\n"
                         "Desktop sessions are authorized and managed by the platform.\n";
            break;
        case Command::kInstallLoginProvider:
            installWindowsLoginProvider(agentExecutablePath().parent_path() / L"ultraterimal_login.dll");
            break;
        case Command::kRemoveLoginProvider:
            removeWindowsLoginProvider(agentExecutablePath().parent_path() / L"ultraterimal_login.dll");
            break;
        case Command::kDesktop: {
            DesktopParent parent(options->windowsSession, options->parentProcess);
            asio::io_context io;
            ruvia::execution_domain attachment(io);
            auto pipe = NamedPipeTransport::connect(io.get_executor(), options->pipeName, options->parentProcess);
            std::exception_ptr failure;
            std::uint32_t exitCode = 1;
            asio::co_spawn(attachment.executor(), runDesktopWorker(attachment.handle(), *pipe, options->windowsSession), [&](std::exception_ptr error, std::uint32_t result) {
                failure = error;
                exitCode = result;
                pipe->cancel();
                attachment.request_stop();
            });
            io.run();
            io.restart();
            auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
            io.run();
            retired.get();
            if (failure) {
                std::rethrow_exception(failure);
            }
            return static_cast<int>(exitCode);
        }
        case Command::kEnroll: {
            const auto config = loadEnrollmentConfig(options->configPath);
            std::string input(4097, '\0');
            std::cin.read(input.data(), static_cast<std::streamsize>(input.size()));
            input.resize(static_cast<std::size_t>(std::cin.gcount()));
            asio::io_context io;
            ruvia::execution_domain attachment(io);
            ConnectionConfig result;
            std::exception_ptr failure;
            asio::co_spawn(attachment.executor(), enrollDevice(attachment.handle(), config, std::move(input)), [&](std::exception_ptr error, ConnectionConfig value) {
                failure = error;
                result = std::move(value);
                attachment.request_stop();
            });
            io.run();
            io.restart();
            auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
            io.run();
            retired.get();
            if (failure) {
                std::rethrow_exception(failure);
            }
            std::cout << connectionConfigJson(result) << '\n';
            break;
        }
        case Command::kConnectionCheck: {
            const auto config = loadConnectionConfig(options->configPath);
            asio::io_context io;
            ruvia::execution_domain attachment(io);
            ControlCheck result;
            std::exception_ptr failure;
            asio::co_spawn(attachment.executor(), checkControlConnection(attachment.handle(), config), [&](std::exception_ptr error, ControlCheck value) {
                failure = error;
                result = value;
                attachment.request_stop();
            });
            io.run();
            io.restart();
            auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
            io.run();
            retired.get();
            if (failure) {
                std::rethrow_exception(failure);
            }
            std::cout << "Control handshake and heartbeat passed; interval=" << result.heartbeatSeconds << "s\n";
            break;
        }
        case Command::kConnect: {
            const auto config = loadConnectionConfig(options->configPath);
            asio::io_context io;
            ruvia::execution_domain attachment(io);
            ControlRuntime runtime(attachment.handle(), config, agentExecutablePath());
            asio::signal_set signals(io, SIGINT, SIGTERM);
            signals.async_wait([&](std::error_code error, int) {
                if (!error) {
                    runtime.stop();
                }
            });
            std::exception_ptr failure;
            asio::co_spawn(attachment.executor(), runtime.run(), [&](std::exception_ptr error) {
                failure = error;
                signals.cancel();
                attachment.request_stop();
            });
            io.run();
            io.restart();
            auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
            io.run();
            retired.get();
            if (failure) {
                std::rethrow_exception(failure);
            }
            break;
        }
        case Command::kVersion:
            std::cout << kAgentVersion << '\n';
            break;
        case Command::kProbe:
            std::cout << capabilityJson(detectCapabilities()) << '\n';
            break;
        case Command::kTerminalCheck: {
            const auto result = checkLocalTerminal();
            if (result.exitCode || !result.markerReceived) {
                throw std::runtime_error("ConPTY diagnostic failed");
            }
            std::cout << "ConPTY diagnostic passed\n";
            break;
        }
        case Command::kStatus:
            std::cout << serviceStatus() << '\n';
            break;
        case Command::kService:
            dispatchService([](std::stop_token stop, ConsoleSessionEvents& consoleEvents) {
                const auto config = loadConnectionConfig(serviceConfigPath());
                asio::io_context io;
                ruvia::execution_domain attachment(io);
                ControlRuntime runtime(attachment.handle(), config, agentExecutablePath(), &consoleEvents);
                std::stop_callback cancellation(stop, [&] { runtime.stop(); });
                std::exception_ptr failure;
                asio::co_spawn(attachment.executor(), runtime.run(), [&](std::exception_ptr error) { failure = error; attachment.request_stop(); });
                io.run();
                io.restart();
                auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
                io.run();
                retired.get();
                if (failure) {
                    std::rethrow_exception(failure);
                }
            });
            break;
        case Command::kInstall:
            installService();
            std::cout << "installed\n";
            break;
        case Command::kUninstall:
            uninstallService();
            std::cout << "uninstalled\n";
            break;
        case Command::kStart:
            startService();
            std::cout << "running\n";
            break;
        case Command::kStop:
            stopService();
            std::cout << "stopped\n";
            break;
        }
        return 0;
    } catch (const std::exception& error) {
        if (!background) {
            std::cerr << "Agent error: " << error.what() << '\n';
        }
        return 1;
    }
}
