#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "platform/windows/system_restart.transport.h"

namespace {
namespace wire = ultraterimal::wire;
using namespace ultraterimal::agent;
class FakePlatform final : public SystemRestartPlatform {
  public:
    bool eligible = true;
    bool alreadyReserved = false;
    std::uint32_t error = 0;
    int persisted = 0;
    int invoked = 0;
    bool isSessionZeroLocalSystem() noexcept override { return eligible; }
    bool persistOperation(std::string_view) override { ++persisted; return !alreadyReserved; }
    std::uint32_t initiatePlannedRestart() noexcept override { ++invoked; return error; }
};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class RegistryFixture {
  public:
    RegistryFixture() {
        path_ = L"Software\\UltraterimalTest\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64());
        require(RegCreateKeyExW(HKEY_CURRENT_USER, path_.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                KEY_ALL_ACCESS, nullptr, &key_, nullptr) == ERROR_SUCCESS, "Could not create registry fixture");
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "Could not open process token");
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        const bool retrieved = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes) != FALSE;
        CloseHandle(token);
        require(retrieved, "Could not read process user SID");
        LPWSTR sidText = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sidText) != FALSE,
                "Could not stringify process user SID");
        trustedSid_ = sidText;
        LocalFree(sidText);
        const std::wstring sddl = L"D:P(A;;KA;;;" + trustedSid_ + L")(A;;KA;;;SY)(A;;KA;;;BA)";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
                "Could not create secure registry fixture ACL");
        PACL dacl = nullptr;
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        const bool hasDacl = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != FALSE && present;
        const DWORD status = hasDacl ? SetSecurityInfo(key_, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                      nullptr, nullptr, dacl, nullptr) : ERROR_INVALID_SECURITY_DESCR;
        LocalFree(descriptor);
        require(status == ERROR_SUCCESS, "Could not protect registry fixture ACL");
    }
    ~RegistryFixture() {
        if (key_) RegCloseKey(key_);
        RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\UltraterimalTest");
    }
    RegistryFixture(const RegistryFixture&) = delete;
    RegistryFixture& operator=(const RegistryFixture&) = delete;
    const std::wstring& path() const { return path_; }
    const std::wstring& trustedSid() const { return trustedSid_; }
    HKEY key() const { return key_; }
  private:
    std::wstring path_;
    std::wstring trustedSid_;
    HKEY key_ = nullptr;
};
void testRegistryPersistence() {
    RegistryFixture fixture;
    RegistryRestartOperationStore store(HKEY_CURRENT_USER, fixture.path(), {fixture.trustedSid(), L"S-1-5-18", L"S-1-5-32-544"});
    require(store.persistOperation(std::string(64, 'c')), "New operation was not persisted");
    require(!store.persistOperation(std::string(64, 'c')), "Existing operation was not deduplicated");

    constexpr BYTE marker[] = {'U', 'T', 'R', 'R', 1};
    require(RegSetValueExW(fixture.key(), (std::wstring(64, L'd')).c_str(), 0, REG_BINARY, marker, sizeof(marker)) == ERROR_SUCCESS,
            "Could not simulate a crash after value write but before flush");
    require(!store.persistOperation(std::string(64, 'd')), "Unflushed operation was not recovered as reserved");

    constexpr BYTE corrupt[] = {0xFF, 0x00};
    require(RegSetValueExW(fixture.key(), (std::wstring(64, L'e')).c_str(), 0, REG_SZ, corrupt, sizeof(corrupt)) == ERROR_SUCCESS,
            "Could not create damaged registry value");
    require(!store.persistOperation(std::string(64, 'e')), "Damaged record was not conservatively deduplicated");
    require(store.persistOperation(std::string(64, 'f')), "Damaged record prevented an unrelated operation");
}
void testUntrustedWritableAclRejected() {
    RegistryFixture fixture;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;KA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
            "Could not create untrusted ACL");
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    require(GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted) != FALSE && daclPresent,
            "Could not inspect low-trust ACL");
    const DWORD aclError = SetSecurityInfo(fixture.key(), SE_REGISTRY_KEY,
                                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                           nullptr, nullptr, dacl, nullptr);
    LocalFree(descriptor);
    require(aclError == ERROR_SUCCESS, "Could not install low-trust ACL fixture");
    RegistryRestartOperationStore store(HKEY_CURRENT_USER, fixture.path(), {fixture.trustedSid(), L"S-1-5-18", L"S-1-5-32-544"});
    bool denied = false;
    try { store.persistOperation(std::string(64, '9')); }
    catch (const std::system_error& error) { denied = error.code().value() == ERROR_ACCESS_DENIED; }
    require(denied, "Writable untrusted ACL was not rejected");
}
wire::WindowsRestartRequest request() {
    wire::WindowsRestartRequest value;
    value.set_operation_id(std::string(64, 'a'));
    value.set_device_id("d-" + std::string(64, 'b'));
    value.set_expires_at_unix_seconds(1300);
    return value;
}
}
int main() {
    try {
        testRegistryPersistence();
        testUntrustedWritableAclRejected();
        constexpr std::uint64_t now = 1000;
        const std::string device = "d-" + std::string(64, 'b');
        auto req = request();
        FakePlatform platform;
        auto result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1001}; });
        require(result.status() == wire::WindowsRestartResult::ACCEPTED && platform.invoked == 1 && platform.persisted == 1,
                "Valid restart was not accepted exactly once");
        platform.alreadyReserved = true;
        result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1001}; });
        require(result.status() == wire::WindowsRestartResult::UNKNOWN && platform.invoked == 1,
                "Duplicate operation was not conservatively reported unknown");
        platform.alreadyReserved = false;
        req = request();
        result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1300}; });
        require(result.status() == wire::WindowsRestartResult::FAILED && result.win32_error() == ERROR_TIMEOUT &&
                    platform.persisted == 3 && platform.invoked == 1,
                "Expired-after-persistence request was not rejected conservatively");
        platform.alreadyReserved = true;
        result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1001}; });
        require(result.status() == wire::WindowsRestartResult::UNKNOWN && platform.invoked == 1,
                "Persisted expired operation ID was not retained as unknown");
        platform.alreadyReserved = false;
        req.set_expires_at_unix_seconds(1301);
        result = handleWindowsRestart(device, req, platform, now);
        require(result.status() == wire::WindowsRestartResult::FAILED && platform.persisted == 4,
                "Long-lived request was not rejected");
        req = request();
        req.set_device_id("d-" + std::string(64, 'c'));
        result = handleWindowsRestart(device, req, platform, now);
        require(result.status() == wire::WindowsRestartResult::FAILED && platform.persisted == 4,
                "Wrong target was not rejected");
        req = request();
        platform.eligible = false;
        result = handleWindowsRestart(device, req, platform, now);
        require(result.status() == wire::WindowsRestartResult::FAILED && platform.persisted == 4,
                "Non-service caller was not rejected");
        platform.eligible = true;
        platform.error = ERROR_SHUTDOWN_IN_PROGRESS;
        result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1001}; });
        require(result.status() == wire::WindowsRestartResult::UNKNOWN && result.win32_error() == ERROR_SHUTDOWN_IN_PROGRESS,
                "Shutdown already in progress was not conservatively reported unknown");
        platform.error = ERROR_ACCESS_DENIED;
        result = handleWindowsRestart(device, req, platform, now, [] { return std::uint64_t{1001}; });
        require(result.status() == wire::WindowsRestartResult::FAILED && result.win32_error() == ERROR_ACCESS_DENIED,
                "Definite restart failure was not reported");
        const auto bootTime = systemBootTimeUnixSeconds();
        Sleep(1100);
        require(systemBootTimeUnixSeconds() == bootTime, "Boot-time estimate changed during the process");
        std::cout << "system restart validation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
