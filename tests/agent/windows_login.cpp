#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include "platform/windows/credential.transport.h"
#include "platform/windows/windows_login.transport.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    using namespace ultraterimal;
    const auto root = std::filesystem::temp_directory_path() / (L"ultraterimal-windows-login-" + std::to_wstring(GetCurrentProcessId()));
    const auto path = root / L"windows_login.credential";
    try {
        require(std::filesystem::create_directory(root), "Test directory already exists");
        wire::WindowsLoginRequest request;
        request.set_request_id(27);
        request.set_action(wire::WindowsLoginRequest::READ);
        auto result = agent::windowsLoginSettings(path, request);
        require(result.success() && !result.saved() && result.request_id() == 27, "Missing credential state");
        request.set_action(wire::WindowsLoginRequest::SAVE);
        require(agent::windowsLoginSettings(path, request).success() && !std::filesystem::exists(path), "Empty save created credentials");
        request.set_username("incomplete");
        require(!agent::windowsLoginSettings(path, request).success(), "Incomplete initial credentials accepted");
        std::array<wchar_t, 257> username{};
        DWORD usernameLength = static_cast<DWORD>(username.size());
        require(GetUserNameW(username.data(), &usernameLength), "Cannot read current test account");
        const auto utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, username.data(), -1, nullptr, 0, nullptr, nullptr);
        require(utf8Length > 1, "Cannot encode test account");
        std::string testUsername(static_cast<std::size_t>(utf8Length), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, username.data(), -1, testUsername.data(), utf8Length, nullptr, nullptr);
        testUsername.pop_back();
        request.set_username(testUsername);
        request.set_domain(".");
        request.set_password("test-only-password-123");
        result = agent::windowsLoginSettings(path, request);
        require(result.success() && result.saved() && result.request_id() == 27, "Credential save failed");
        for (const auto scenario : {CPUS_LOGON, CPUS_UNLOCK_WORKSTATION}) {
            auto login = agent::prepareSavedWindowsLogon(path, scenario);
            require(login.has_value() && login->buffer.size() != 0, "Saved credential cannot be serialized");
            require(login->account.ends_with(username.data()) && login->sid.starts_with(L"S-1-"), "Prepared account lacks a Windows identity");
            const std::string_view bytes(reinterpret_cast<const char*>(login->buffer.data()), login->buffer.size());
            require(bytes.find(request.password()) == std::string_view::npos, "Plain password escaped service");
        }
        require(!agent::prepareSavedWindowsLogon(path, CPUS_CREDUI), "Saved credential accepted UAC scenario");
        std::ifstream file(path, std::ios::binary);
        const std::string encrypted((std::istreambuf_iterator<char>(file)), {});
        file.close();
        require(encrypted.find(request.password()) == std::string::npos && encrypted.find(request.username()) == std::string::npos, "Plaintext stored on disk");
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS, "Cannot inspect credential permissions");
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        const bool protectedAcl = GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) && acl && acl->AceCount == 2;
        LocalFree(descriptor);
        require(protectedAcl, "Credential file inherited broad permissions");
        auto read = request;
        read.clear_username();
        read.clear_domain();
        read.clear_password();
        read.set_action(wire::WindowsLoginRequest::READ);
        require(ImpersonateAnonymousToken(GetCurrentThread()) != FALSE, "Cannot create unrelated security context");
        const bool unauthorizedRead = agent::windowsLoginSettings(path, read).success();
        const bool unauthorizedSerialize = agent::prepareSavedWindowsLogon(path, CPUS_LOGON).has_value();
        require(RevertToSelf() != FALSE, "Cannot restore test security context");
        require(!unauthorizedRead, "Unrelated security context read Windows credentials");
        require(!unauthorizedSerialize, "Unrelated security context serialized Windows credentials");
        request.set_username("updated-user");
        request.clear_password();
        request.clear_domain();
        require(agent::windowsLoginSettings(path, request).success(), "Partial account update failed");
        wire::WindowsLoginRequest stored;
        require(stored.ParseFromString(agent::loadCredential(path)) && stored.username() == "updated-user" && stored.password() == "test-only-password-123" && stored.domain() == ".", "Blank fields erased existing credentials");
        const auto lastWrite = std::filesystem::last_write_time(path);
        request.clear_username();
        require(agent::windowsLoginSettings(path, request).success() && std::filesystem::last_write_time(path) == lastWrite, "Empty update rewrote stored credentials");
        request.set_password(std::string(1025, 'x'));
        require(!agent::windowsLoginSettings(path, request).success(), "Oversized password accepted");
        request.Clear();
        request.set_request_id(31);
        request.set_action(wire::WindowsLoginRequest::READ);
        result = agent::windowsLoginSettings(path, request);
        require(result.success() && result.saved() && result.username() == "updated-user" && result.domain() == ".", "Rejected save corrupted stored account");
        require(result.SerializeAsString().find("password") == std::string::npos, "Password echoed in result");
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "invalid DPAPI";
        corrupt.close();
        require(!agent::windowsLoginSettings(path, request).success(), "Corrupt credential accepted");
        require(!agent::prepareSavedWindowsLogon(path, CPUS_LOGON), "Corrupt credential serialized");
        request.set_action(wire::WindowsLoginRequest::CLEAR);
        require(agent::windowsLoginSettings(path, request).success() && !std::filesystem::exists(path), "Clear did not remove credential");
        require(agent::windowsLoginSettings(path, request).success(), "Clear is not idempotent");
        require(!agent::prepareSavedWindowsLogon(path, CPUS_LOGON), "Cleared credential still serialized");
        request.set_action(wire::WindowsLoginRequest::SAVE);
        request.set_username("new-user");
        request.set_password("test-only-new-password");
        require(agent::windowsLoginSettings(path, request).success(), "Cannot create initial concurrent test credentials");
        auto passwordUpdate = request;
        passwordUpdate.clear_username();
        passwordUpdate.set_password("concurrent-password");
        request.clear_password();
        request.set_username("concurrent-user");
        bool firstSaved = false, secondSaved = false;
        std::thread first([&] { firstSaved = agent::windowsLoginSettings(path, request).success(); });
        std::thread second([&] { secondSaved = agent::windowsLoginSettings(path, passwordUpdate).success(); });
        first.join();
        second.join();
        require(firstSaved && secondSaved, "Concurrent partial updates failed");
        require(stored.ParseFromString(agent::loadCredential(path)) && stored.username() == "concurrent-user" && stored.password() == "concurrent-password", "Concurrent merge lost an updated field");
        request.clear_username();
        request.clear_password();
        request.set_action(wire::WindowsLoginRequest::CLEAR);
        require(agent::windowsLoginSettings(path, request).success(), "Cannot clear reconfigured credentials");
        require(!agent::windowsLoginSettings(root / "missing" / "file", request).success(), "Failed operation reported success");
        require(std::filesystem::is_empty(root), "Temporary credential file leaked");
        std::filesystem::remove(root);
        std::cout << "Windows credential encryption, permissions, partial updates, no-op, concurrent merge and corruption passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
