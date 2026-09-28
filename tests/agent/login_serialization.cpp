#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <ntsecapi.h>
#include <wincred.h>

#include "platform/windows/login_serialization.transport.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::wstring unpack(const ultraterimal::agent::WindowsLogonBuffer& buffer, const UNICODE_STRING& text) {
    const auto offset = reinterpret_cast<std::uintptr_t>(text.Buffer);
    require(offset >= sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) && offset <= buffer.size() && text.Length <= buffer.size() - offset &&
                text.Length % sizeof(wchar_t) == 0 && text.MaximumLength == text.Length,
            "Invalid serialized string bounds");
    return {reinterpret_cast<const wchar_t*>(buffer.data() + offset), text.Length / sizeof(wchar_t)};
}
} // namespace
int main() {
    using ultraterimal::agent::serializeWindowsLogon;
    try {
        for (const auto scenario : {CPUS_LOGON, CPUS_UNLOCK_WORKSTATION}) {
            auto result = serializeWindowsLogon(L"test-domain", L"test-user", L"test-only-password", scenario);
            require(result.has_value(), "Cannot serialize protected login");
            const auto& packed = *reinterpret_cast<const KERB_INTERACTIVE_UNLOCK_LOGON*>(result->data());
            const std::span<const BYTE> record(result->data(), result->size());
            auto imported = ultraterimal::agent::importWindowsLogon(record, result->package(), scenario);
            require(imported && imported->size() == result->size() && std::memcmp(imported->data(), result->data(), result->size()) == 0, "Protected IPC serialization failed round-trip");
            require(!ultraterimal::agent::importWindowsLogon(record, result->package() + 1, scenario), "Foreign authentication package accepted");
            require(!ultraterimal::agent::importWindowsLogon(record.first(record.size() - 1), result->package(), scenario), "Truncated logon accepted");
            require(!ultraterimal::agent::importWindowsLogon(record, result->package(), CPUS_CREDUI), "IPC accepted UAC scenario");
            std::vector<BYTE> malformed(record.begin(), record.end());
            KERB_INTERACTIVE_UNLOCK_LOGON invalid = packed;
            invalid.Logon.UserName.Buffer = reinterpret_cast<PWSTR>(~std::uintptr_t{});
            std::memcpy(malformed.data(), &invalid, sizeof(invalid));
            require(!ultraterimal::agent::importWindowsLogon(malformed, result->package(), scenario), "IPC accepted out-of-bounds username");
            invalid = packed;
            invalid.LogonId.LowPart = 1;
            std::memcpy(malformed.data(), &invalid, sizeof(invalid));
            require(!ultraterimal::agent::importWindowsLogon(malformed, result->package(), scenario), "IPC accepted caller-selected logon session");
            require(packed.Logon.MessageType == (scenario == CPUS_LOGON ? KerbInteractiveLogon : KerbWorkstationUnlockLogon), "Wrong logon scenario");
            require(unpack(*result, packed.Logon.LogonDomainName) == L"test-domain" && unpack(*result, packed.Logon.UserName) == L"test-user", "Account changed");
            auto password = unpack(*result, packed.Logon.Password);
            require(password != L"test-only-password", "Password serialized without protection");
            CRED_PROTECTION_TYPE protection = CredUnprotected;
            require(CredIsProtectedW(password.data(), &protection) && protection != CredUnprotected, "Password is not Windows-protected");
            DWORD count = 0;
            require(!CredUnprotectW(TRUE, password.data(), static_cast<DWORD>(password.size() + 1), nullptr, &count) && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Cannot size unprotected password");
            std::vector<wchar_t> recovered(count);
            require(CredUnprotectW(TRUE, password.data(), static_cast<DWORD>(password.size() + 1), recovered.data(), &count) != FALSE, "Cannot recover protected test password");
            const bool equal = std::wstring_view(recovered.data()) == L"test-only-password";
            SecureZeroMemory(recovered.data(), recovered.size() * sizeof(wchar_t));
            require(equal, "Protected password did not round-trip");
            auto moved = std::move(*result);
            require(result->data() == nullptr && result->size() == 0, "Move retained buffer ownership");
            const auto serialized = moved.release(CLSID_NULL);
            require(serialized.rgbSerialization && serialized.cbSerialization && moved.data() == nullptr, "Cannot transfer LogonUI buffer ownership");
            SecureZeroMemory(serialized.rgbSerialization, serialized.cbSerialization);
            CoTaskMemFree(serialized.rgbSerialization);
        }
        require(!serializeWindowsLogon(L"", L"user", L"password", CPUS_CREDUI), "UAC credential UI accepted");
        require(!serializeWindowsLogon(L"", L"user", L"password", CPUS_CHANGE_PASSWORD), "Password change accepted");
        require(!serializeWindowsLogon(L"", L"", L"password", CPUS_LOGON), "Empty account accepted");
        require(!serializeWindowsLogon(L"", L"user", std::wstring(513, L'x'), CPUS_LOGON), "Oversized password accepted");
        require(!serializeWindowsLogon(L"", L"user", std::wstring_view(L"one\0two", 7), CPUS_LOGON), "Embedded zero accepted");
        std::cout << "Protected Windows logon/unlock serialization and ownership passed; no OS login attempted\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
