#include <iostream>
#include <stdexcept>
#include <thread>

#include "platform/windows/login_provider.transport.h"

namespace {
using namespace ultraterimal::agent;
using Microsoft::WRL::ComPtr;
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void count(WindowsLoginProvider& provider, bool present) {
    DWORD count = 99, selected = 99;
    BOOL automatic = TRUE;
    require(provider.GetCredentialCount(&count, &selected, &automatic) == S_OK, "Enumeration failed");
    require(count == (present ? 1U : 0U) && selected == (present ? 0U : CREDENTIAL_PROVIDER_NO_DEFAULT) && (automatic != FALSE) == present,
            "Incorrect automatic credential enumeration");
}
HRESULT publish(WindowsLoginProvider& provider, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario = CPUS_LOGON, ULONGLONG expiry = 0) {
    auto buffer = serializeWindowsLogon(L"test", L"user", L"test-only-password", CPUS_LOGON);
    require(buffer.has_value(), "Cannot serialize test credential");
    return provider.publish(std::move(*buffer), L"user", L"S-1-5-18", scenario, expiry ? expiry : GetTickCount64() + 30000);
}
void expectRevoked(ICredentialProviderCredential& credential) {
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE response{};
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION data{};
    PWSTR text = nullptr;
    CREDENTIAL_PROVIDER_STATUS_ICON icon{};
    require(credential.GetSerialization(&response, &data, &text, &icon) == S_OK && !data.rgbSerialization &&
                response == CPGSR_NO_CREDENTIAL_NOT_FINISHED,
            "Retained tile submitted revoked credential");
}
class Events final : public ICredentialProviderEvents {
  public:
    ULONG references = 1;
    unsigned changes = 0;
    UINT_PTR context = 0;
    WindowsLoginProvider* unadvise = nullptr;
    HRESULT result = S_OK;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) {
            return E_POINTER;
        }
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(ICredentialProviderEvents)) {
            return E_NOINTERFACE;
        }
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE CredentialsChanged(UINT_PTR value) override {
        ++changes;
        context = value;
        return unadvise ? unadvise->UnAdvise() : result;
    }
};
} // namespace
int main() {
    try {
        Events events;
        ComPtr<WindowsLoginProvider> provider;
        provider.Attach(new WindowsLoginProvider);
        count(*provider.Get(), false);
        require(publish(*provider.Get()) == E_INVALIDARG, "Uninitialized scenario accepted");
        require(provider->SetUsageScenario(CPUS_CREDUI, 0) == E_NOTIMPL, "UAC scenario accepted");
        require(provider->SetUsageScenario(CPUS_LOGON, 0) == S_OK, "Logon scenario rejected");
        require(publish(*provider.Get(), CPUS_UNLOCK_WORKSTATION) == E_INVALIDARG, "Scenario mismatch accepted");
        require(publish(*provider.Get(), CPUS_LOGON, GetTickCount64()) == E_INVALIDARG, "Expired delivery accepted");
        require(publish(*provider.Get(), CPUS_LOGON, GetTickCount64() + 60000) == E_INVALIDARG, "Unbounded delivery accepted");
        require(provider->Advise(&events, 37) == S_OK, "Advise failed");
        require(publish(*provider.Get()) == S_OK && events.changes == 1 && events.context == 37, "Missing enumeration notification");
        count(*provider.Get(), true);
        ComPtr<ICredentialProviderCredential> retained;
        require(provider->GetCredentialAt(0, &retained) == S_OK, "Missing tile");
        require(provider->revoke() == S_OK && events.changes == 2, "Missing revocation notification");
        count(*provider.Get(), false);
        expectRevoked(*retained.Get());
        require(publish(*provider.Get()) == S_OK, "Republish failed");
        retained.Reset();
        require(provider->GetCredentialAt(0, &retained) == S_OK, "Missing replacement tile");
        require(provider->SetUsageScenario(CPUS_CHANGE_PASSWORD, 0) == E_NOTIMPL, "Change-password accepted");
        expectRevoked(*retained.Get());
        count(*provider.Get(), false);
        require(provider->SetUsageScenario(CPUS_LOGON, 0) == S_OK, "Reset scenario failed");
        HRESULT crossThread = S_OK;
        std::thread worker([&] { crossThread = provider->revoke(); });
        worker.join();
        require(crossThread == RPC_E_WRONG_THREAD, "Cross-thread mutation accepted");
        DWORD fields = 0;
        require(provider->GetFieldDescriptorCount(&fields) == S_OK && fields == 2, "Wrong field count");
        for (DWORD index = 0; index < fields; ++index) {
            CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* field = nullptr;
            require(provider->GetFieldDescriptorAt(index, &field) == S_OK && field && field->dwFieldID == index && field->pszLabel,
                    "Invalid field descriptor");
            CoTaskMemFree(field->pszLabel);
            CoTaskMemFree(field);
        }
        const auto notifications = events.changes;
        require(publish(*provider.Get(), CPUS_LOGON, GetTickCount64() + 100) == S_OK, "Timed request failed");
        ComPtr<ICredentialProviderCredential> timed;
        require(provider->GetCredentialAt(0, &timed) == S_OK, "Missing timed credential");
        const auto deadline = GetTickCount64() + 2000;
        // Dispatch this test thread's message-only window without invoking
        // enumeration, so lazy expiry cannot masquerade as timer notification.
        while (events.changes < notifications + 2 && GetTickCount64() < deadline) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        require(events.changes == notifications + 2, "Expiry did not actively notify LogonUI");
        count(*provider.Get(), false);
        expectRevoked(*timed.Get());
        timed.Reset();
        events.result = E_FAIL;
        require(publish(*provider.Get()) == E_FAIL, "Notification failure was hidden");
        count(*provider.Get(), false);
        events.result = S_OK;
        events.unadvise = provider.Get();
        require(publish(*provider.Get()) == S_OK && events.references == 1, "Reentrant UnAdvise leaked event reference");
        count(*provider.Get(), false);
        provider.Reset();
        require(!WindowsLoginModuleReference::unused(), "Retained credential failed to retain module");
        retained.Reset();
        require(WindowsLoginModuleReference::unused(), "Provider or credential leaked module reference");
        // The earlier lifecycle warms up User32 and credential protection before
        // measuring repeated creation of message-only windows and expiry timers.
        DWORD handlesBefore = 0, handlesAfter = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), "Cannot read baseline handles");
        const auto userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        for (unsigned cycle = 0; cycle < 128; ++cycle) {
            ComPtr<WindowsLoginProvider> instance;
            instance.Attach(new WindowsLoginProvider);
            require(instance->SetUsageScenario(CPUS_LOGON, 0) == S_OK, "Cycle scenario failed");
            require(publish(*instance.Get()) == S_OK, "Cycle publish failed");
            ComPtr<ICredentialProviderCredential> tile;
            require(instance->GetCredentialAt(0, &tile) == S_OK, "Cycle tile missing");
            instance.Reset();
            expectRevoked(*tile.Get());
            tile.Reset();
            require(WindowsLoginModuleReference::unused(), "Cycle retained module reference");
        }
        require(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter), "Cannot read final handles");
        require(handlesAfter <= handlesBefore, "Provider cycles leaked kernel handles");
        require(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= userBefore, "Provider cycles leaked User32 objects");
        std::cout << "Provider scenario, enumeration, revocation, thread ownership and event lifetime passed; not registered with Windows\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
