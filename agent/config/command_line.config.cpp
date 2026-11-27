#include "config/command_line.config.h"

#include <array>
#include <utility>

namespace ultraterimal::agent {
namespace {
std::uint32_t identifier(std::wstring_view text) {
    std::uint64_t value = 0;
    if (text.empty() || text.size() > 10) {
        return 0;
    }
    for (const auto digit : text) {
        if (digit < L'0' || digit > L'9') {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(digit - L'0');
        if (value >= 0xffffffff) {
            return 0;
        }
    }
    return static_cast<std::uint32_t>(value);
}
} // namespace
std::expected<CommandLine, std::wstring> parseCommandLine(std::span<const std::wstring_view> arguments) {
    if (arguments.empty()) {
        return CommandLine{};
    }
    if (arguments.size() == 7 && arguments[0] == L"--desktop" && arguments[1] == L"--session" && arguments[3] == L"--pipe" && arguments[5] == L"--parent") {
        const auto session = identifier(arguments[2]), parent = identifier(arguments[6]);
        if (!session || !parent || arguments[4].empty() || arguments[4].size() > 128) {
            return std::unexpected(L"Invalid desktop bootstrap identifiers.");
        }
        CommandLine options;
        options.command = Command::kDesktop;
        options.pipeName = arguments[4];
        options.windowsSession = session;
        options.parentProcess = parent;
        return options;
    }
    if (arguments.size() == 2 && !arguments[1].empty()) {
        if (arguments.front() == L"--enroll") {
            CommandLine result;
            result.command = Command::kEnroll;
            result.configPath = std::filesystem::path(arguments[1]);
            return result;
        }
        if (arguments.front() == L"--connection-check" || arguments.front() == L"--connect") {
            CommandLine result;
            result.command = arguments.front() == L"--connect" ? Command::kConnect : Command::kConnectionCheck;
            result.configPath = std::filesystem::path(arguments[1]);
            return result;
        }
    }
    if (arguments.size() != 1) {
        return std::unexpected(L"Expected exactly one command; use --help.");
    }
    constexpr std::array commands{
        std::pair{L"--help", Command::kHelp}, std::pair{L"--version", Command::kVersion},
        std::pair{L"--apply-update", Command::kApplyUpdate},
        std::pair{L"--probe", Command::kProbe}, std::pair{L"--service", Command::kService},
        std::pair{L"--install", Command::kInstall}, std::pair{L"--uninstall", Command::kUninstall},
        std::pair{L"--install-login-provider", Command::kInstallLoginProvider}, std::pair{L"--remove-login-provider", Command::kRemoveLoginProvider},
        std::pair{L"--start", Command::kStart}, std::pair{L"--stop", Command::kStop},
        std::pair{L"--status", Command::kStatus}, std::pair{L"--terminal-check", Command::kTerminalCheck}};
    for (const auto& [name, command] : commands) {
        if (arguments.front() == name) {
            CommandLine result;
            result.command = command;
            return result;
        }
    }
    return std::unexpected(L"Unknown command; use --help.");
}
std::expected<CommandLine, std::string> parseCommandLine(std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        return CommandLine{};
    }
    if (arguments.size() == 2 && !arguments[1].empty()) {
        if (arguments.front() == "--enroll") {
            CommandLine result;
            result.command = Command::kEnroll;
            result.configPath = std::filesystem::path(std::u8string(arguments[1].begin(), arguments[1].end()));
            return result;
        }
        if (arguments.front() == "--connection-check" || arguments.front() == "--connect") {
            CommandLine result;
            result.command = arguments.front() == "--connect" ? Command::kConnect : Command::kConnectionCheck;
            result.configPath = std::filesystem::path(std::u8string(arguments[1].begin(), arguments[1].end()));
            return result;
        }
    }
    if (arguments.size() != 1) {
        return std::unexpected(std::string("Expected exactly one command; use --help."));
    }
    constexpr std::array commands{
        std::pair{"--help", Command::kHelp}, std::pair{"--version", Command::kVersion},
        std::pair{"--apply-update", Command::kApplyUpdate},
        std::pair{"--probe", Command::kProbe}, std::pair{"--service", Command::kService},
        std::pair{"--install", Command::kInstall}, std::pair{"--uninstall", Command::kUninstall},
        std::pair{"--start", Command::kStart}, std::pair{"--stop", Command::kStop},
        std::pair{"--status", Command::kStatus}, std::pair{"--terminal-check", Command::kTerminalCheck}};
    for (const auto& [name, command] : commands) {
        if (arguments.front() == name) {
            CommandLine result;
            result.command = command;
            return result;
        }
    }
    return std::unexpected(std::string("Unknown command; use --help."));
}
} // namespace ultraterimal::agent
