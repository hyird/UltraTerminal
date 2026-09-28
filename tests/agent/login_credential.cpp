#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "platform/windows/login_credential.transport.h"

namespace {
using namespace ultraterimal::agent;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct ReleaseCredential {
    void operator()(ICredentialProviderCredential2* value) const { value->Release(); }
};
auto credential(const std::shared_ptr<WindowsLoginAttempt>& attempt) {
    auto buffer = serializeWindowsLogon(L"test-domain", L"test-user", L"test-only-password", CPUS_LOGON);
    require(buffer.has_value(), "Serialization failed");
    ICredentialProviderCredential2* value = nullptr;
    require(createWindowsLoginCredential(std::move(*buffer), L"test-user", L"S-1-5-18", attempt, &value) == S_OK, "Credential creation failed");
    return std::unique_ptr<ICredentialProviderCredential2, ReleaseCredential>(value);
}
void serialize(ICredentialProviderCredential2& value, bool expected) {
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response{};
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION buffer{};
    PWSTR text = nullptr;
    CREDENTIAL_PROVIDER_STATUS_ICON icon{};
    require(value.GetSerialization(&response, &buffer, &text, &icon) == S_OK, "GetSerialization failed");
    require(text == nullptr && icon == CPSI_NONE, "Unexpected status");
    if (expected) {
        require(response == CPGSR_RETURN_CREDENTIAL_FINISHED && buffer.rgbSerialization && buffer.cbSerialization &&
                    buffer.clsidCredentialProvider == kWindowsLoginProviderId,
                "Missing or misattributed credential");
        SecureZeroMemory(buffer.rgbSerialization, buffer.cbSerialization);
        CoTaskMemFree(buffer.rgbSerialization);
    } else {
        require(response == CPGSR_NO_CREDENTIAL_NOT_FINISHED && !buffer.rgbSerialization && !buffer.cbSerialization,
                "Unauthorized or repeated submission");
    }
}
} // namespace
int main() {
    try {
        const auto pipeName = L"\\\\.\\pipe\\UltraterimalLoginLeaseTest-" + std::to_wstring(GetCurrentProcessId());
        UniqueHandle server(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                             PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr));
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(server && event, "Cannot create test authorization pipe");
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        require(!ConnectNamedPipe(server.get(), &operation) && GetLastError() == ERROR_IO_PENDING, "Cannot wait for authorization peer");
        UniqueHandle client(CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        DWORD count = 0;
        require(client && GetOverlappedResult(server.get(), &operation, &count, TRUE), "Cannot connect authorization peer");
        auto lease = std::make_shared<WindowsLoginAttempt>(GetTickCount64() + 30000, std::move(server));
        auto leasedCredential = credential(lease);
        require(lease->available(), "Live authorization pipe was rejected");
        client.reset();
        require(!lease->available(), "Disconnected lease remains available before UI dispatch");
        serialize(*leasedCredential, false);
        auto attempt = std::make_shared<WindowsLoginAttempt>(GetTickCount64() + 30000);
        auto value = credential(attempt);
        BOOL automatic = FALSE;
        require(value->SetSelected(&automatic) == S_OK && automatic, "Valid attempt not selected");
        serialize(*value, true);
        serialize(*value, false);
        require(value->SetSelected(&automatic) == S_OK && !automatic, "Consumed attempt selected");
        PWSTR text = nullptr;
        CREDENTIAL_PROVIDER_STATUS_ICON icon{};
        require(value->ReportResult(static_cast<NTSTATUS>(0xc000006dL), 0, &text, &icon) == S_OK && text && icon == CPSI_ERROR,
                "Failure not reported");
        CoTaskMemFree(text);
        serialize(*value, false);
        auto rejected = std::make_shared<WindowsLoginAttempt>(GetTickCount64() + 30000);
        auto tile = credential(rejected);
        rejected->cancel();
        serialize(*tile, false);
        WindowsLoginAttempt expired(GetTickCount64());
        auto expiredBuffer = serializeWindowsLogon(L"", L"user", L"test-only-password", CPUS_LOGON);
        require(expiredBuffer.has_value() && !expired.bind(std::move(*expiredBuffer)) && !expired.take().data(), "Expired attempt accepted a buffer");
        auto deselected = credential(std::make_shared<WindowsLoginAttempt>(GetTickCount64() + 30000));
        require(deselected->SetDeselected() == S_OK, "Cannot deselect");
        serialize(*deselected, false);
        WindowsLoginAttempt concurrent(GetTickCount64() + 30000);
        auto concurrentBuffer = serializeWindowsLogon(L"", L"user", L"test-only-password", CPUS_LOGON);
        require(concurrentBuffer.has_value() && concurrent.bind(std::move(*concurrentBuffer)), "Cannot bind concurrent test buffer");
        std::atomic<int> winners{0};
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < 16; ++i) {
                threads.emplace_back([&] { winners += concurrent.take().data() ? 1 : 0; });
            }
        }
        require(winners == 1 && !concurrent.available(), "Concurrent duplicate consumption");
        std::cout << "Credential single-use, expiry, cancellation, deselection and failure checks passed; no OS login attempted\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
