#pragma once
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace ultraterimal::agent {
enum class Command { kHelp,
                     kVersion,
                     kProbe,
                     kService,
                     kInstall,
                     kUninstall,
                     kInstallLoginProvider,
                     kRemoveLoginProvider,
                     kStart,
                     kStop,
                     kStatus,
                     kTerminalCheck,
                     kConnectionCheck,
                     kConnect,
                     kEnroll,
                     kApplyUpdate,
                     kDesktop };
struct CommandLine {
    Command command = Command::kHelp;
    std::filesystem::path configPath;
    std::wstring pipeName;
    std::uint32_t windowsSession = 0;
    std::uint32_t parentProcess = 0;
};
std::expected<CommandLine, std::wstring> parseCommandLine(std::span<const std::wstring_view> arguments);
} // namespace ultraterimal::agent
