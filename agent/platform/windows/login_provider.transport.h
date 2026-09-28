#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <windows.h>

#include <credentialprovider.h>
#include <wrl/client.h>

#include "login_credential.transport.h"
#include "login_inbox.transport.h"
#include "login_module.runtime.h"

namespace ultraterimal::agent {
// All methods except IUnknown reference counting run on the creating apartment.
// The IPC owner must marshal deliveries to that apartment; no worker calls events.
class WindowsLoginProvider final : public ICredentialProvider, public ICredentialProviderSetUserArray {
  public:
    WindowsLoginProvider() noexcept;
    HRESULT publish(WindowsLogonBuffer buffer, std::wstring account, std::wstring sid,
                    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, ULONGLONG expiresAt, UniqueHandle servicePipe = UniqueHandle{}) noexcept;
    HRESULT revoke() noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION*) override;
    HRESULT STDMETHODCALLTYPE Advise(ICredentialProviderEvents* events, UINT_PTR context) override;
    HRESULT STDMETHODCALLTYPE UnAdvise() override;
    HRESULT STDMETHODCALLTYPE GetFieldDescriptorCount(DWORD* count) override;
    HRESULT STDMETHODCALLTYPE GetFieldDescriptorAt(DWORD index, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** descriptor) override;
    HRESULT STDMETHODCALLTYPE GetCredentialCount(DWORD* count, DWORD* selected, BOOL* automatic) override;
    HRESULT STDMETHODCALLTYPE GetCredentialAt(DWORD index, ICredentialProviderCredential** credential) override;
    HRESULT STDMETHODCALLTYPE SetUserArray(ICredentialProviderUserArray* users) override;

  private:
    ~WindowsLoginProvider();
    bool ownerThread() const noexcept;
    void clear() noexcept;
    HRESULT changed() noexcept;
    HRESULT ensureWindow() noexcept;
    bool armExpiry() noexcept;
    void startInbox() noexcept;
    void receiveLogin() noexcept;
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
    WindowsLoginModuleReference module_;
    const DWORD thread_;
    std::atomic<ULONG> references_{1};
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario_ = CPUS_INVALID;
    Microsoft::WRL::ComPtr<ICredentialProviderEvents> events_;
    UINT_PTR context_ = 0;
    Microsoft::WRL::ComPtr<ICredentialProviderCredential2> credential_;
    std::shared_ptr<WindowsLoginAttempt> attempt_;
    HINSTANCE instance_ = nullptr;
    ATOM windowClass_ = 0;
    HWND window_ = nullptr;
    ULONGLONG expiresAt_ = 0;
    std::unique_ptr<WindowsLoginInbox> inbox_;
};
} // namespace ultraterimal::agent
