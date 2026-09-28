#include "login_serialization.transport.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <ntsecapi.h>
#include <wincred.h>

namespace ultraterimal::agent {
namespace {
struct SecretCharacters {
    std::vector<wchar_t> value;
    ~SecretCharacters() { SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
};
bool validText(std::wstring_view value, std::size_t maximum, bool allowEmpty = false) {
    return (allowEmpty || !value.empty()) && value.size() <= maximum && value.find(L'\0') == std::wstring_view::npos;
}
std::expected<ULONG, HRESULT> negotiatePackage() {
    LSA_HANDLE connection = nullptr;
    auto status = LsaConnectUntrusted(&connection);
    if (status < 0) {
        return std::unexpected(HRESULT_FROM_WIN32(LsaNtStatusToWinError(status)));
    }
    char name[] = "Negotiate";
    LSA_STRING packageName{static_cast<USHORT>(sizeof(name) - 1), static_cast<USHORT>(sizeof(name)), name};
    ULONG package = 0;
    status = LsaLookupAuthenticationPackage(connection, &packageName, &package);
    LsaDeregisterLogonProcess(connection);
    if (status < 0) {
        return std::unexpected(HRESULT_FROM_WIN32(LsaNtStatusToWinError(status)));
    }
    return package;
}
} // namespace
std::expected<WindowsLogonBuffer, HRESULT> importWindowsLogon(std::span<const BYTE> bytes, ULONG package, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept {
    if ((scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) || bytes.size() < sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) || bytes.size() > 20000) {
        return std::unexpected(E_INVALIDARG);
    }
    try {
        KERB_INTERACTIVE_UNLOCK_LOGON packed{};
        std::memcpy(&packed, bytes.data(), sizeof(packed));
        const auto expectedType = scenario == CPUS_LOGON ? KerbInteractiveLogon : KerbWorkstationUnlockLogon;
        if (packed.Logon.MessageType != expectedType || packed.LogonId.LowPart || packed.LogonId.HighPart) {
            return std::unexpected(E_INVALIDARG);
        }
        std::size_t offset = sizeof(packed);
        const auto valid = [&](const UNICODE_STRING& text, std::size_t maximum, bool empty) {
            if (reinterpret_cast<std::uintptr_t>(text.Buffer) != offset || text.Length != text.MaximumLength ||
                text.Length % sizeof(wchar_t) || text.Length > maximum * sizeof(wchar_t) ||
                (!empty && !text.Length) || text.Length > bytes.size() - offset) {
                return false;
            }
            for (std::size_t index = 0; index < text.Length; index += sizeof(wchar_t)) {
                wchar_t value = 0;
                std::memcpy(&value, bytes.data() + offset + index, sizeof(value));
                if (!value) {
                    return false;
                }
            }
            offset += text.Length;
            return true;
        };
        if (!valid(packed.Logon.LogonDomainName, 128, true) || !valid(packed.Logon.UserName, 256, false) ||
            !valid(packed.Logon.Password, 8192, false) || offset != bytes.size()) {
            return std::unexpected(E_INVALIDARG);
        }
        const auto expectedPackage = negotiatePackage();
        if (!expectedPackage || *expectedPackage != package) {
            return std::unexpected(E_INVALIDARG);
        }
        SecretCharacters password;
        password.value.resize(packed.Logon.Password.Length / sizeof(wchar_t) + 1);
        std::memcpy(password.value.data(), bytes.data() + reinterpret_cast<std::uintptr_t>(packed.Logon.Password.Buffer), packed.Logon.Password.Length);
        CRED_PROTECTION_TYPE protection = CredUnprotected;
        if (!CredIsProtectedW(password.value.data(), &protection) || protection == CredUnprotected) {
            return std::unexpected(E_ACCESSDENIED);
        }
        auto* allocation = static_cast<BYTE*>(CoTaskMemAlloc(bytes.size()));
        if (!allocation) {
            return std::unexpected(E_OUTOFMEMORY);
        }
        std::memcpy(allocation, bytes.data(), bytes.size());
        return WindowsLogonBuffer(allocation, static_cast<ULONG>(bytes.size()), package);
    } catch (const std::bad_alloc&) {
        return std::unexpected(E_OUTOFMEMORY);
    } catch (...) {
        return std::unexpected(E_FAIL);
    }
}
WindowsLogonBuffer::WindowsLogonBuffer(BYTE* data, ULONG size, ULONG package) noexcept : data_(data), size_(size), package_(package) {}
WindowsLogonBuffer::~WindowsLogonBuffer() { clear(); }
WindowsLogonBuffer::WindowsLogonBuffer(WindowsLogonBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)), package_(std::exchange(other.package_, 0)) {}
WindowsLogonBuffer& WindowsLogonBuffer::operator=(WindowsLogonBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        package_ = std::exchange(other.package_, 0);
    }
    return *this;
}
void WindowsLogonBuffer::clear() noexcept {
    if (data_) {
        SecureZeroMemory(data_, size_);
        CoTaskMemFree(data_);
    }
    data_ = nullptr;
    size_ = 0;
    package_ = 0;
}
CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION WindowsLogonBuffer::release(const CLSID& provider) noexcept {
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION result{};
    result.ulAuthenticationPackage = std::exchange(package_, 0);
    result.clsidCredentialProvider = provider;
    result.cbSerialization = std::exchange(size_, 0);
    result.rgbSerialization = std::exchange(data_, nullptr);
    return result;
}
std::expected<WindowsLogonBuffer, HRESULT> serializeWindowsLogon(std::wstring_view domain, std::wstring_view username,
                                                                 std::wstring_view password, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept {
    if ((scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) || !validText(domain, 128, true) ||
        !validText(username, 256) || !validText(password, 512)) {
        return std::unexpected(E_INVALIDARG);
    }
    try {
        const auto package = negotiatePackage();
        if (!package) {
            return std::unexpected(package.error());
        }
        SecretCharacters plain;
        plain.value.resize(password.size() + 1);
        std::memcpy(plain.value.data(), password.data(), password.size() * sizeof(wchar_t));
        DWORD protectedSize = 0;
        if (CredProtectW(TRUE, plain.value.data(), static_cast<DWORD>(plain.value.size()), nullptr, &protectedSize, nullptr) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || protectedSize < 2 || protectedSize > 8192) {
            return std::unexpected(E_FAIL);
        }
        SecretCharacters protectedPassword;
        protectedPassword.value.resize(protectedSize);
        if (!CredProtectW(TRUE, plain.value.data(), static_cast<DWORD>(plain.value.size()), protectedPassword.value.data(), &protectedSize, nullptr)) {
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto terminator = std::find(protectedPassword.value.begin(), protectedPassword.value.end(), L'\0');
        if (terminator == protectedPassword.value.end()) {
            return std::unexpected(E_FAIL);
        }
        const auto passwordLength = static_cast<std::size_t>(terminator - protectedPassword.value.begin());
        const auto bytes = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON) + (domain.size() + username.size() + passwordLength) * sizeof(wchar_t);
        auto* allocation = static_cast<BYTE*>(CoTaskMemAlloc(bytes));
        if (!allocation) {
            return std::unexpected(E_OUTOFMEMORY);
        }
        WindowsLogonBuffer result(allocation, static_cast<ULONG>(bytes), *package);
        std::memset(allocation, 0, bytes);
        auto& logon = reinterpret_cast<KERB_INTERACTIVE_UNLOCK_LOGON*>(allocation)->Logon;
        logon.MessageType = scenario == CPUS_LOGON ? KerbInteractiveLogon : KerbWorkstationUnlockLogon;
        std::size_t offset = sizeof(KERB_INTERACTIVE_UNLOCK_LOGON);
        const auto append = [&](UNICODE_STRING& target, std::wstring_view value) {
            target.Length = static_cast<USHORT>(value.size() * sizeof(wchar_t));
            target.MaximumLength = target.Length;
            target.Buffer = reinterpret_cast<PWSTR>(offset);
            if (!value.empty()) {
                std::memcpy(allocation + offset, value.data(), target.Length);
            }
            offset += target.Length;
        };
        append(logon.LogonDomainName, domain);
        append(logon.UserName, username);
        append(logon.Password, std::wstring_view(protectedPassword.value.data(), passwordLength));
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(E_OUTOFMEMORY);
    } catch (...) {
        return std::unexpected(E_FAIL);
    }
}
} // namespace ultraterimal::agent
