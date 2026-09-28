#include "system_restart.transport.h"

#include <array>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

namespace ultraterimal::agent {
namespace {
constexpr std::uint64_t kMaximumLifetimeSeconds = 300;
constexpr DWORD kShutdownReason = SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE | SHTDN_REASON_FLAG_PLANNED;
constexpr wchar_t kRestartKey[] = L"SOFTWARE\\Ultraterimal\\SystemRestart";
constexpr ACCESS_MASK kRegistryWriteRights = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_CREATE_LINK | DELETE | WRITE_DAC | WRITE_OWNER;
std::mutex gRestartRegistryMutex;

bool validOperationId(std::string_view value) noexcept {
    if (value.size() != 64) return false;
    for (const char digit : value) if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
    return true;
}
bool validDeviceId(std::string_view value) noexcept { return value.size() == 66 && value.starts_with("d-") && validOperationId(value.substr(2)); }
wire::WindowsRestartResult resultFor(const wire::WindowsRestartRequest& request, wire::WindowsRestartResult::Status status, std::uint32_t error = 0) {
    wire::WindowsRestartResult result;
    result.set_operation_id(request.operation_id());
    result.set_status(status);
    result.set_win32_error(error);
    return result;
}
class TokenHandle {
  public:
    explicit TokenHandle(HANDLE value) : value_(value) {}
    ~TokenHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const { return value_; }
  private:
    HANDLE value_;
};
class RegistryKey {
  public:
    explicit RegistryKey(HKEY value = nullptr) : value_(value) {}
    ~RegistryKey() { if (value_) RegCloseKey(value_); }
    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;
    RegistryKey(RegistryKey&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    HKEY get() const { return value_; }
    HKEY* receive() { return &value_; }
  private:
    HKEY value_ = nullptr;
};
void throwRegistryError(LSTATUS status, const char* action) {
    throw std::system_error(static_cast<int>(status), std::system_category(), action);
}
bool trustedWriterSid(PSID sid, const std::vector<std::wstring>& trustedSidStrings) noexcept {
    for (const auto& sidString : trustedSidStrings) {
        PSID trustedSid = nullptr;
        if (ConvertStringSidToSidW(sidString.c_str(), &trustedSid)) {
            const bool matches = EqualSid(sid, trustedSid) != FALSE;
            LocalFree(trustedSid);
            if (matches) return true;
        }
    }
    return false;
}
void validateRestartKey(HKEY key, const std::vector<std::wstring>& trustedSidStrings) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityError = GetSecurityInfo(key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                                &owner, nullptr, &dacl, nullptr, &descriptor);
    if (securityError != ERROR_SUCCESS) throw std::system_error(securityError, std::system_category(), "Inspect restart registry security");
    struct DescriptorGuard { PSECURITY_DESCRIPTOR value; ~DescriptorGuard() { LocalFree(value); } } guard{descriptor};
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (!owner || !trustedWriterSid(owner, trustedSidStrings) || !GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted) || !daclPresent || !dacl) {
        throw std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "Untrusted restart registry key");
    }
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* rawAce = nullptr;
        if (!GetAce(dacl, index, &rawAce)) throw std::system_error(GetLastError(), std::system_category(), "Inspect restart registry ACL");
        const auto* header = static_cast<ACE_HEADER*>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_ALLOWED_OBJECT_ACE_TYPE) continue;
        ACCESS_MASK mask = 0;
        PSID sid = nullptr;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
        } else {
            const auto* ace = static_cast<const ACCESS_ALLOWED_OBJECT_ACE*>(rawAce);
            mask = ace->Mask;
            sid = const_cast<DWORD*>(&ace->SidStart);
            if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) sid = reinterpret_cast<BYTE*>(sid) + sizeof(GUID);
            if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) sid = reinterpret_cast<BYTE*>(sid) + sizeof(GUID);
        }
        GENERIC_MAPPING mapping{KEY_READ, KEY_WRITE, KEY_READ | KEY_WRITE, KEY_ALL_ACCESS};
        MapGenericMask(&mask, &mapping);
        if ((mask & (kRegistryWriteRights | KEY_WRITE | GENERIC_WRITE | GENERIC_ALL)) != 0 && !trustedWriterSid(sid, trustedSidStrings)) {
            throw std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "Writable restart registry key has untrusted principal");
        }
    }
    DWORD type = 0;
    BYTE linkTarget[sizeof(wchar_t)]{};
    DWORD bytes = sizeof(linkTarget);
    const LSTATUS linkStatus = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, linkTarget, &bytes);
    if (linkStatus == ERROR_SUCCESS && type == REG_LINK) throw std::system_error(ERROR_ACCESS_DENIED, std::system_category(), "Restart registry key is a link");
}
RegistryKey openRestartKey(HKEY rootKey, const std::wstring& keyPath, const std::vector<std::wstring>& trustedSidStrings) {
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(rootKey, keyPath.c_str(), REG_OPTION_OPEN_LINK, READ_CONTROL | KEY_QUERY_VALUE | KEY_SET_VALUE, key.receive());
    if (status == ERROR_FILE_NOT_FOUND) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        std::wstring sddl = L"D:P";
        for (const auto& sid : trustedSidStrings) sddl += L"(A;OICI;KA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            throw std::system_error(GetLastError(), std::system_category(), "Create restart registry ACL");
        }
        struct DescriptorGuard { PSECURITY_DESCRIPTOR value; ~DescriptorGuard() { LocalFree(value); } } guard{descriptor};
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        DWORD disposition = 0;
        status = RegCreateKeyExW(rootKey, keyPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE | READ_CONTROL, &security, key.receive(), &disposition);
    }
    if (status != ERROR_SUCCESS) throwRegistryError(status, "Open restart registry key");
    validateRestartKey(key.get(), trustedSidStrings);
    return key;
}
class RestartImpersonation {
  public:
    RestartImpersonation() = default;
    RestartImpersonation(const RestartImpersonation&) = delete;
    RestartImpersonation& operator=(const RestartImpersonation&) = delete;
    ~RestartImpersonation() { finish(); if (token_) CloseHandle(token_); }
    DWORD begin() noexcept {
        HANDLE existingToken = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &existingToken)) { CloseHandle(existingToken); return ERROR_ACCESS_DENIED; }
        const DWORD threadTokenError = GetLastError();
        if (threadTokenError != ERROR_NO_TOKEN) return threadTokenError;
        HANDLE processToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &processToken)) return GetLastError();
        TokenHandle source(processToken);
        if (!DuplicateTokenEx(source.get(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token_)) return GetLastError();
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &requested.Privileges[0].Luid)) return GetLastError();
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD previousSize = sizeof(previous_);
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(token_, FALSE, &requested, sizeof(previous_), &previous_, &previousSize)) return GetLastError();
        adjusted_ = true;
        if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) return ERROR_NOT_ALL_ASSIGNED;
        if (!SetThreadToken(nullptr, token_)) return GetLastError();
        impersonating_ = true;
        return ERROR_SUCCESS;
    }
    DWORD finish() noexcept {
        DWORD error = ERROR_SUCCESS;
        if (adjusted_) {
            SetLastError(ERROR_SUCCESS);
            if (!AdjustTokenPrivileges(token_, FALSE, &previous_, 0, nullptr, nullptr) || GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
                error = GetLastError();
                if (error == ERROR_SUCCESS) error = ERROR_NOT_ALL_ASSIGNED;
            }
            adjusted_ = false;
        }
        if (impersonating_) { if (!RevertToSelf()) std::terminate(); impersonating_ = false; }
        return error;
    }
  private:
    HANDLE token_ = nullptr;
    TOKEN_PRIVILEGES previous_{};
    bool adjusted_ = false;
    bool impersonating_ = false;
};
std::uint64_t currentUnixSeconds() noexcept {
    constexpr std::uint64_t kWindowsToUnixEpochSeconds = 11644473600ULL;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    const auto value = ticks.QuadPart / 10000000ULL;
    return value > kWindowsToUnixEpochSeconds ? value - kWindowsToUnixEpochSeconds : 0;
}
class NativeSystemRestartPlatform final : public SystemRestartPlatform {
  private:
    RegistryRestartOperationStore store_{HKEY_LOCAL_MACHINE, kRestartKey, {L"S-1-5-18", L"S-1-5-32-544"}};
  public:
    bool isSessionZeroLocalSystem() noexcept override {
        DWORD session = MAXDWORD;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &session) == FALSE || session != 0) return false;
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        TokenHandle token(raw);
        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) return false;
        std::vector<std::byte> storage(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes)) return false;
        return IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, WinLocalSystemSid) != FALSE;
    }
    bool persistOperation(std::string_view operationId) override {
        return store_.persistOperation(operationId);
    }
    std::uint32_t initiatePlannedRestart() noexcept override {
        RestartImpersonation scope;
        const DWORD beginError = scope.begin();
        if (beginError != ERROR_SUCCESS) { const DWORD restoreError = scope.finish(); return restoreError == ERROR_SUCCESS ? beginError : ERROR_SHUTDOWN_IN_PROGRESS; }
        const BOOL accepted = InitiateSystemShutdownExW(nullptr, nullptr, 0, FALSE, TRUE, kShutdownReason);
        const DWORD restartError = accepted ? ERROR_SUCCESS : GetLastError();
        const DWORD restoreError = scope.finish();
        if (restoreError != ERROR_SUCCESS) return ERROR_SHUTDOWN_IN_PROGRESS;
        return restartError;
    }
};
} // namespace

RegistryRestartOperationStore::RegistryRestartOperationStore(void* rootKey, std::wstring keyPath,
                                                             std::vector<std::wstring> trustedSidStrings)
    : rootKey_(rootKey), keyPath_(std::move(keyPath)), trustedSidStrings_(std::move(trustedSidStrings)) {}

bool RegistryRestartOperationStore::persistOperation(std::string_view operationId) const {
    if (!validOperationId(operationId)) throw std::system_error(ERROR_INVALID_PARAMETER, std::system_category(), "Invalid restart operation");
    std::scoped_lock lock(gRestartRegistryMutex);
    auto key = openRestartKey(static_cast<HKEY>(rootKey_), keyPath_, trustedSidStrings_);
    const std::wstring valueName(operationId.begin(), operationId.end());
    DWORD existingType = 0;
    const LSTATUS query = RegQueryValueExW(key.get(), valueName.c_str(), nullptr, &existingType, nullptr, nullptr);
    if (query == ERROR_SUCCESS) return false;
    if (query != ERROR_FILE_NOT_FOUND) throwRegistryError(query, "Query restart operation");
    constexpr BYTE marker[] = { 'U', 'T', 'R', 'R', 1 };
    const LSTATUS written = RegSetValueExW(key.get(), valueName.c_str(), 0, REG_BINARY, marker, sizeof(marker));
    if (written != ERROR_SUCCESS) throwRegistryError(written, "Write restart operation");
    const LSTATUS flushed = RegFlushKey(key.get());
    if (flushed != ERROR_SUCCESS) throwRegistryError(flushed, "Flush restart operation");
    return true;
}

wire::WindowsRestartResult handleWindowsRestart(std::string_view deviceId, const wire::WindowsRestartRequest& request,
                                                SystemRestartPlatform& platform, std::uint64_t nowUnixSeconds,
                                                std::function<std::uint64_t()> postPersistNow) {
    if (!validOperationId(request.operation_id()) || !validDeviceId(deviceId) || request.device_id() != deviceId ||
        request.expires_at_unix_seconds() <= nowUnixSeconds || request.expires_at_unix_seconds() - nowUnixSeconds > kMaximumLifetimeSeconds) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, ERROR_INVALID_PARAMETER);
    }
    if (!platform.isSessionZeroLocalSystem()) return resultFor(request, wire::WindowsRestartResult::FAILED, ERROR_ACCESS_DENIED);
    try {
        if (!platform.persistOperation(request.operation_id())) return resultFor(request, wire::WindowsRestartResult::UNKNOWN, ERROR_ALREADY_EXISTS);
    } catch (const std::system_error& error) {
        return resultFor(request, wire::WindowsRestartResult::FAILED, static_cast<std::uint32_t>(error.code().value()));
    } catch (...) { return resultFor(request, wire::WindowsRestartResult::FAILED, ERROR_WRITE_FAULT); }
    const auto effectiveNow = postPersistNow ? postPersistNow() : currentUnixSeconds();
    if (request.expires_at_unix_seconds() <= effectiveNow) return resultFor(request, wire::WindowsRestartResult::FAILED, ERROR_TIMEOUT);
    const auto error = platform.initiatePlannedRestart();
    if (error == ERROR_SHUTDOWN_IN_PROGRESS) return resultFor(request, wire::WindowsRestartResult::UNKNOWN, error);
    if (error != ERROR_SUCCESS) return resultFor(request, wire::WindowsRestartResult::FAILED, error);
    return resultFor(request, wire::WindowsRestartResult::ACCEPTED);
}
SystemRestartPlatform& nativeSystemRestartPlatform() { static NativeSystemRestartPlatform platform; return platform; }
std::uint64_t systemBootTimeUnixSeconds() noexcept {
    static const std::uint64_t value = [] {
        constexpr std::uint64_t kWindowsToUnixEpochSeconds = 11644473600ULL;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = now.dwLowDateTime;
        ticks.HighPart = now.dwHighDateTime;
        const auto uptimeSeconds = GetTickCount64() / 1000;
        const auto nowSeconds = ticks.QuadPart / 10000000ULL;
        return nowSeconds > kWindowsToUnixEpochSeconds + uptimeSeconds ? nowSeconds - kWindowsToUnixEpochSeconds - uptimeSeconds : 0;
    }();
    return value;
}
} // namespace ultraterimal::agent
