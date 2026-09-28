#include "login_install.transport.h"

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>

#include <windows.h>

#include "login_credential.transport.h"

namespace ultraterimal::agent {
namespace {
struct Key {
    HKEY value = nullptr;
    ~Key() {
        if (value) {
            RegCloseKey(value);
        }
    }
};
void require(LSTATUS status) {
    if (status != ERROR_SUCCESS) {
        throw std::system_error(static_cast<int>(status), std::system_category(), "Windows login provider registration");
    }
}
std::wstring classId() {
    std::array<wchar_t, 40> text{};
    if (!StringFromGUID2(kWindowsLoginProviderId, text.data(), static_cast<int>(text.size()))) {
        throw std::runtime_error("Cannot format login provider identity");
    }
    return text.data();
}
std::wstring classPath() { return L"SOFTWARE\\Classes\\CLSID\\" + classId(); }
std::wstring providerPath() { return L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential Providers\\" + classId(); }
void setText(HKEY key, const wchar_t* name, const std::wstring& value) {
    require(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}
bool exists(const std::wstring& path) {
    Key key;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND) {
        return false;
    }
    require(status);
    return true;
}
void erase(const std::wstring& path) {
    // Open the 64-bit parent explicitly; never recursively delete a parent of
    // this provider's CLSID, or another provider's registration.
    const auto separator = path.find_last_of(L'\\');
    Key parent;
    const auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.substr(0, separator).c_str(), 0, KEY_WRITE | KEY_READ | KEY_WOW64_64KEY, &parent.value);
    if (status == ERROR_FILE_NOT_FOUND) {
        return;
    }
    require(status);
    const auto removed = RegDeleteTreeW(parent.value, path.c_str() + separator + 1);
    if (removed != ERROR_FILE_NOT_FOUND) {
        require(removed);
    }
}
} // namespace
void installWindowsLoginProvider(const std::filesystem::path& module) {
    if (!module.is_absolute() || !std::filesystem::is_regular_file(module)) {
        throw std::runtime_error("Login provider DLL is missing");
    }
    const auto clsid = classPath(), provider = providerPath();
    if (exists(clsid) || exists(provider)) {
        throw std::runtime_error("Login provider is already registered");
    }
    bool createdClass = false, createdProvider = false;
    try {
        Key root;
        DWORD disposition = 0;
        require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsid.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &root.value, &disposition));
        if (disposition != REG_CREATED_NEW_KEY) {
            throw std::runtime_error("Login provider registration changed during installation");
        }
        createdClass = true;
        setText(root.value, nullptr, L"Ultraterimal Windows Login");
        Key inproc;
        require(RegCreateKeyExW(root.value, L"InprocServer32", 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &inproc.value, nullptr));
        setText(inproc.value, nullptr, module.native());
        setText(inproc.value, L"ThreadingModel", L"Apartment");
        Key registration;
        require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, provider.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &registration.value, &disposition));
        if (disposition != REG_CREATED_NEW_KEY) {
            throw std::runtime_error("Login provider registration changed during installation");
        }
        createdProvider = true;
        setText(registration.value, nullptr, L"Ultraterimal Windows Login");
    } catch (...) {
        if (createdProvider) {
            erase(provider);
        }
        if (createdClass) {
            erase(clsid);
        }
        throw;
    }
}
void removeWindowsLoginProvider(const std::filesystem::path& module) {
    const auto clsid = classPath(), provider = providerPath();
    if (!exists(clsid) && !exists(provider)) {
        return;
    }
    Key inproc;
    require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, (clsid + L"\\InprocServer32").c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &inproc.value));
    std::array<wchar_t, 32768> stored{};
    DWORD bytes = static_cast<DWORD>(stored.size() * sizeof(wchar_t)), kind = 0;
    require(RegQueryValueExW(inproc.value, nullptr, nullptr, &kind, reinterpret_cast<BYTE*>(stored.data()), &bytes));
    if (kind != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) || bytes > stored.size() * sizeof(wchar_t) ||
        stored[bytes / sizeof(wchar_t) - 1] != L'\0' ||
        CompareStringOrdinal(stored.data(), static_cast<int>(bytes / sizeof(wchar_t) - 1), module.c_str(), static_cast<int>(module.native().size()), TRUE) != CSTR_EQUAL) {
        throw std::runtime_error("Login provider registration belongs to a different module");
    }
    erase(provider);
    erase(clsid);
}
} // namespace ultraterimal::agent
