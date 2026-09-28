#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <windows.h>

#include "platform/windows/login_install.transport.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
struct RegistrySandbox {
    std::wstring path = L"Software\\UltraterimalLoginInstallTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY key = nullptr;
    bool overridden = false;
    RegistrySandbox() {
        DWORD disposition = 0;
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, &disposition) == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY, "Cannot create registry test sandbox");
        overridden = RegOverridePredefKey(HKEY_LOCAL_MACHINE, key) == ERROR_SUCCESS;
        require(overridden, "Cannot redirect HKLM to test sandbox");
    }
    ~RegistrySandbox() {
        if (overridden) {
            RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        }
        if (key) {
            RegCloseKey(key);
        }
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected provider DLL path");
        RegistrySandbox sandbox;
        const auto module = std::filesystem::absolute(argv[1]);
        using namespace ultraterimal::agent;
        installWindowsLoginProvider(module);
        bool duplicateRejected = false;
        try {
            installWindowsLoginProvider(module);
        } catch (...) {
            duplicateRejected = true;
        }
        require(duplicateRejected, "Registration overwrote an existing provider");
        bool foreignRejected = false;
        try {
            removeWindowsLoginProvider(module.parent_path() / "unrelated.dll");
        } catch (...) {
            foreignRejected = true;
        }
        require(foreignRejected, "Uninstall removed a different provider module");
        removeWindowsLoginProvider(module);
        removeWindowsLoginProvider(module);
        installWindowsLoginProvider(module);
        removeWindowsLoginProvider(module);
        std::cout << "Provider registration, duplicate ownership, uninstall and reinstall passed in redirected HKLM; system registration unchanged\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
