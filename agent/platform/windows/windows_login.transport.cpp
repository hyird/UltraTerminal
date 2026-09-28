#include "windows_login.transport.h"

#include <array>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

#include "credential.transport.h"

namespace ultraterimal::agent {
namespace {
struct Secret {
    std::string bytes;
    ~Secret() { SecureZeroMemory(bytes.data(), bytes.size()); }
};
struct RequestSecret {
    wire::WindowsLoginRequest& request;
    ~RequestSecret() {
        auto* password = request.mutable_password();
        SecureZeroMemory(password->data(), password->size());
    }
};
struct WideSecret {
    std::vector<wchar_t> value;
    ~WideSecret() { SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); }
    std::wstring_view view() const { return {value.data(), value.size()}; }
};
bool decodeText(std::string_view input, WideSecret& output) {
    if (input.empty()) {
        return true;
    }
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) {
        return false;
    }
    output.value.resize(count);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.value.data(), count) == count;
}
bool validStoredRequest(wire::WindowsLoginRequest& request) {
    if (request.action() != wire::WindowsLoginRequest::SAVE || request.username().empty() || request.password().empty()) {
        return false;
    }
    wire::Envelope validation;
    validation.set_protocol_version(protocol::kVersion);
    validation.set_sequence(1);
    validation.mutable_windows_login_request()->Swap(&request);
    const auto valid = protocol::validateMessage(validation).has_value();
    validation.mutable_windows_login_request()->Swap(&request);
    return valid;
}
struct TemporaryFile {
    std::filesystem::path path;
    ~TemporaryFile() {
        if (!path.empty()) {
            DeleteFileW(path.c_str());
        }
    }
};
void restrictAccess(const std::filesystem::path& path) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;OW)", SDDL_REVISION_1, &raw, nullptr)) {
        throw std::runtime_error("Cannot create Windows credential permissions");
    }
    const auto release = [](void* value) { LocalFree(value); };
    const std::unique_ptr<void, decltype(release)> descriptor(raw, release);
    if (!SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, raw)) {
        throw std::runtime_error("Cannot protect Windows credential permissions");
    }
}
void save(const std::filesystem::path& path, std::string_view bytes) {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Cannot allocate Windows credential file");
    }
    std::wstring suffix = L".pending-";
    for (const auto value : random) {
        suffix += L"0123456789abcdef"[value >> 4];
        suffix += L"0123456789abcdef"[value & 15];
    }
    TemporaryFile temporary{std::filesystem::path(path.wstring() + suffix)};
    saveCredential(temporary.path, bytes);
    restrictAccess(temporary.path);
    if (!MoveFileExW(temporary.path.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING)) {
        throw std::runtime_error("Cannot save Windows credential file");
    }
    temporary.path.clear();
}
} // namespace
wire::WindowsLoginResult windowsLoginSettings(const std::filesystem::path& path, wire::WindowsLoginRequest request) {
    RequestSecret secret{request};
    const auto requestId = request.request_id();
    wire::WindowsLoginResult result;
    result.set_request_id(request.request_id());
    try {
        // Serialize read/merge/write so concurrent partial updates preserve
        // fields written by the preceding request.
        static std::mutex settingsMutex;
        const std::lock_guard lock(settingsMutex);
        wire::Envelope validation;
        validation.set_protocol_version(protocol::kVersion);
        validation.set_sequence(1);
        validation.mutable_windows_login_request()->Swap(&request);
        const bool valid = protocol::validateMessage(validation).has_value();
        validation.mutable_windows_login_request()->Swap(&request);
        if (!valid || !path.is_absolute()) {
            return result;
        }
        if (request.action() == wire::WindowsLoginRequest::CLEAR) {
            if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
                return result;
            }
        } else if (request.action() == wire::WindowsLoginRequest::SAVE) {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) {
                if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                    return result;
                }
                Secret plain{loadCredential(path)};
                wire::WindowsLoginRequest stored;
                RequestSecret storedSecret{stored};
                if (!stored.ParseFromString(plain.bytes) || !validStoredRequest(stored)) {
                    return result;
                }
                if (request.username().empty()) {
                    request.set_username(stored.username());
                }
                if (request.domain().empty()) {
                    request.set_domain(stored.domain());
                }
                if (request.password().empty()) {
                    request.set_password(stored.password());
                }
                if (request.username() == stored.username() && request.domain() == stored.domain() && request.password() == stored.password()) {
                    result.set_success(true);
                    result.set_saved(true);
                    result.set_username(stored.username());
                    result.set_domain(stored.domain());
                    return result;
                }
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                return result;
            } else if (request.username().empty() && request.domain().empty() && request.password().empty()) {
                result.set_success(true);
                return result;
            }
            if (!validStoredRequest(request)) {
                return result;
            }
            // Persist only the account, not a stale remote request identifier.
            request.set_request_id(1);
            Secret plain{request.SerializeAsString()};
            save(path, plain.bytes);
            result.set_saved(true);
            result.set_username(request.username());
            result.set_domain(request.domain());
        } else {
            const auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                    return result;
                }
            } else {
                if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                    return result;
                }
                Secret plain{loadCredential(path)};
                wire::WindowsLoginRequest stored;
                RequestSecret storedSecret{stored};
                if (!stored.ParseFromString(plain.bytes) || !validStoredRequest(stored)) {
                    return result;
                }
                result.set_saved(true);
                result.set_username(stored.username());
                result.set_domain(stored.domain());
            }
        }
        result.set_success(true);
    } catch (...) {
        result.Clear();
        result.set_request_id(requestId);
    }
    return result;
}
std::expected<PreparedWindowsLogon, HRESULT> prepareSavedWindowsLogon(const std::filesystem::path& path, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept {
    if (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) {
        return std::unexpected(E_INVALIDARG);
    }
    try {
        if (!path.is_absolute()) {
            return std::unexpected(E_INVALIDARG);
        }
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        }
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            return std::unexpected(E_ACCESSDENIED);
        }
        Secret plain{loadCredential(path)};
        wire::WindowsLoginRequest stored;
        RequestSecret storedSecret{stored};
        if (!stored.ParseFromString(plain.bytes) || !validStoredRequest(stored)) {
            return std::unexpected(E_INVALIDARG);
        }
        WideSecret domain, username, password;
        if (!decodeText(stored.domain(), domain) || !decodeText(stored.username(), username) || !decodeText(stored.password(), password)) {
            return std::unexpected(E_INVALIDARG);
        }
        std::wstring account;
        auto logonDomain = domain.view();
        auto logonUsername = username.view();
        const auto separator = logonUsername.find(L'\\');
        if (separator != std::wstring_view::npos) {
            if (!logonDomain.empty() || separator == 0 || separator + 1 == logonUsername.size() ||
                logonUsername.find(L'\\', separator + 1) != std::wstring_view::npos) {
                return std::unexpected(E_INVALIDARG);
            }
            logonDomain = logonUsername.substr(0, separator);
            logonUsername.remove_prefix(separator + 1);
        }
        std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> computer{};
        DWORD computerLength = static_cast<DWORD>(computer.size());
        if ((logonDomain.empty() && logonUsername.find(L'@') == std::wstring_view::npos) || logonDomain == L".") {
            if (!GetComputerNameW(computer.data(), &computerLength)) {
                return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
            }
            logonDomain = {computer.data(), computerLength};
        }
        if (!logonDomain.empty()) {
            account.assign(logonDomain);
            account += L'\\';
        }
        account += logonUsername;
        DWORD sidSize = SECURITY_MAX_SID_SIZE, referencedSize = 256;
        alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
        std::array<wchar_t, 256> referenced{};
        SID_NAME_USE kind{};
        if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sidSize, referenced.data(), &referencedSize, &kind)) {
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        }
        if (kind != SidTypeUser || !IsValidSid(sid.data())) {
            return std::unexpected(E_ACCESSDENIED);
        }
        LPWSTR rawSid = nullptr;
        if (!ConvertSidToStringSidW(sid.data(), &rawSid)) {
            return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto release = [](wchar_t* value) { LocalFree(value); };
        const std::unique_ptr<wchar_t, decltype(release)> ownedSid(rawSid, release);
        auto buffer = serializeWindowsLogon(logonDomain, logonUsername, password.view(), scenario);
        if (!buffer) {
            return std::unexpected(buffer.error());
        }
        return PreparedWindowsLogon{std::move(*buffer), std::move(account), std::wstring(ownedSid.get())};
    } catch (const std::bad_alloc&) {
        return std::unexpected(E_OUTOFMEMORY);
    } catch (...) {
        return std::unexpected(E_FAIL);
    }
}
} // namespace ultraterimal::agent
