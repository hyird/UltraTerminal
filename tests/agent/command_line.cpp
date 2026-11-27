#include <array>
#include <iostream>
#include <string_view>

#include "config/command_line.config.h"

int main() {
    using namespace ultraterimal::agent;
    using namespace std::literals;
    if (!parseCommandLine(std::span<const std::wstring_view>{}) ||
        parseCommandLine(std::span<const std::wstring_view>{})->command != Command::kHelp ||
        !parseCommandLine(std::span<const std::string_view>{}) ||
        parseCommandLine(std::span<const std::string_view>{})->command != Command::kHelp) {
        return 1;
    }
    const std::array install{L"--install"sv};
    const std::array installLogin{L"--install-login-provider"sv};
    const std::array removeLogin{L"--remove-login-provider"sv};
    if (!parseCommandLine(installLogin) || parseCommandLine(installLogin)->command != Command::kInstallLoginProvider ||
        !parseCommandLine(removeLogin) || parseCommandLine(removeLogin)->command != Command::kRemoveLoginProvider) {
        return 14;
    }
    const std::array enroll{L"--enroll"sv, L"bootstrap.json"sv};
    if (!parseCommandLine(enroll) || parseCommandLine(enroll)->command != Command::kEnroll) {
        return 9;
    }
    const std::array connect{L"--connect"sv, L"settings.json"sv};
    const auto parsed = parseCommandLine(connect);
    if (!parsed || parsed->command != Command::kConnect || parsed->configPath != L"settings.json") {
        return 7;
    }
    const std::array missingConfig{L"--connect"sv};
    if (parseCommandLine(missingConfig)) {
        return 8;
    }
    if (!parseCommandLine(install) || parseCommandLine(install)->command != Command::kInstall) {
        return 2;
    }
    const std::array conflict{L"--install"sv, L"--uninstall"sv};
    if (parseCommandLine(conflict)) {
        return 3;
    }
    const std::array unknown{L"--desktop"sv};
    if (parseCommandLine(unknown)) {
        return 4;
    }
    std::array desktop{L"--desktop"sv, L"--session"sv, L"7"sv, L"--pipe"sv, L"pipe-fixture"sv, L"--parent"sv, L"123"sv};
    const auto worker = parseCommandLine(desktop);
    if (!worker || worker->command != Command::kDesktop || worker->windowsSession != 7 || worker->parentProcess != 123 || worker->pipeName != L"pipe-fixture") {
        return 10;
    }
    for (auto invalid : {L"0"sv, L"-1"sv, L"4294967295"sv, L"4294967296"sv, L"7x"sv}) {
        desktop[2] = invalid;
        if (parseCommandLine(desktop)) {
            return 11;
        }
    }
    desktop[2] = L"7"sv;
    desktop[0] = L"--capture"sv;
    if (parseCommandLine(desktop)) {
        return 13;
    }
    desktop[0] = L"--desktop"sv;
    desktop[6] = L"0"sv;
    if (parseCommandLine(desktop)) {
        return 12;
    }
    const std::array empty{L""sv};
    if (parseCommandLine(empty)) {
        return 5;
    }
    const std::array extra{L"--probe"sv, L"secret"sv};
    const auto error = parseCommandLine(extra);
    if (error || error.error().find(L"secret") != std::wstring::npos) {
        return 6;
    }
    const std::array utf8Connect{"--connect"sv, "linux.json"sv};
    const auto parsedUtf8 = parseCommandLine(std::span<const std::string_view>{utf8Connect});
    if (!parsedUtf8 || parsedUtf8->command != Command::kConnect || parsedUtf8->configPath != "linux.json") {
        return 15;
    }
    const std::array utf8Bad{"--connect"sv};
    if (parseCommandLine(std::span<const std::string_view>{utf8Bad})) {
        return 16;
    }
    std::cout << "Command validation passed\n";
}
