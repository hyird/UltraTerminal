#include "login_credential.transport.h"

#include <atomic>
#include <cstring>
#include <new>
#include <optional>
#include <utility>

#include <sddl.h>

#include "login_module.runtime.h"

namespace ultraterimal::agent {
struct WindowsLoginAttempt::State {
    struct Guard {
        SRWLOCK& lock;
        explicit Guard(SRWLOCK& value) noexcept : lock(value) { AcquireSRWLockExclusive(&lock); }
        ~Guard() { ReleaseSRWLockExclusive(&lock); }
    };
    SRWLOCK lock = SRWLOCK_INIT;
    const ULONGLONG expiresAt;
    bool active = true, bound = false;
    std::optional<WindowsLogonBuffer> buffer;
    UniqueHandle servicePipe;
    const bool leased;
    explicit State(ULONGLONG deadline, UniqueHandle pipe) : expiresAt(deadline), servicePipe(std::move(pipe)), leased(static_cast<bool>(servicePipe)) {}
    bool live() const noexcept {
        return active && GetTickCount64() < expiresAt && (!leased || PeekNamedPipe(servicePipe.get(), nullptr, 0, nullptr, nullptr, nullptr));
    }
};
WindowsLoginAttempt::WindowsLoginAttempt(ULONGLONG expiresAt, UniqueHandle servicePipe) : state_(std::make_unique<State>(expiresAt, std::move(servicePipe))) {}
WindowsLoginAttempt::~WindowsLoginAttempt() = default;
bool WindowsLoginAttempt::available() const noexcept {
    const State::Guard guard(state_->lock);
    return state_->live() && state_->buffer.has_value();
}
bool WindowsLoginAttempt::bind(WindowsLogonBuffer buffer) noexcept {
    const State::Guard guard(state_->lock);
    if (!buffer.data() || !buffer.size() || !state_->live() || state_->bound) {
        return false;
    }
    state_->bound = true;
    state_->buffer.emplace(std::move(buffer));
    return true;
}
WindowsLogonBuffer WindowsLoginAttempt::take() noexcept {
    const State::Guard guard(state_->lock);
    if (!state_->live() || !state_->buffer) {
        state_->active = false;
        state_->buffer.reset();
        return {nullptr, 0, 0};
    }
    state_->active = false;
    auto result = std::move(*state_->buffer);
    state_->buffer.reset();
    return result;
}
void WindowsLoginAttempt::cancel() noexcept {
    const State::Guard guard(state_->lock);
    state_->active = false;
    state_->buffer.reset();
    state_->servicePipe.reset();
}
namespace {
HRESULT copyText(std::wstring_view text, PWSTR* output) noexcept {
    if (!output) {
        return E_POINTER;
    }
    *output = nullptr;
    const auto size = (text.size() + 1) * sizeof(wchar_t);
    auto* memory = static_cast<PWSTR>(CoTaskMemAlloc(size));
    if (!memory) {
        return E_OUTOFMEMORY;
    }
    std::memcpy(memory, text.data(), text.size() * sizeof(wchar_t));
    memory[text.size()] = L'\0';
    *output = memory;
    return S_OK;
}
class LoginCredential final : public ICredentialProviderCredential2 {
  public:
    LoginCredential(std::wstring account, std::wstring sid, std::shared_ptr<WindowsLoginAttempt> attempt)
        : account_(std::move(account)), sid_(std::move(sid)), attempt_(std::move(attempt)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ICredentialProviderCredential) || id == __uuidof(ICredentialProviderCredential2)) {
            *output = static_cast<ICredentialProviderCredential2*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Advise(ICredentialProviderCredentialEvents*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UnAdvise() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSelected(BOOL* automatic) override {
        if (!automatic) {
            return E_POINTER;
        }
        *automatic = attempt_->available();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDeselected() override {
        attempt_->cancel();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFieldState(DWORD field, CREDENTIAL_PROVIDER_FIELD_STATE* state, CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* interactive) override {
        if (!state || !interactive) {
            return E_POINTER;
        }
        if (field > 1) {
            return E_INVALIDARG;
        }
        *state = CPFS_DISPLAY_IN_SELECTED_TILE;
        *interactive = CPFIS_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStringValue(DWORD field, PWSTR* text) override {
        if (field > 1) {
            return E_INVALIDARG;
        }
        return copyText(field == 0 ? std::wstring_view(account_) : std::wstring_view(L"使用已保存的凭据登录"), text);
    }
    HRESULT STDMETHODCALLTYPE GetBitmapValue(DWORD, HBITMAP*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCheckboxValue(DWORD, BOOL*, PWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSubmitButtonValue(DWORD, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetComboBoxValueCount(DWORD, DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetComboBoxValueAt(DWORD, DWORD, PWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStringValue(DWORD, PCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCheckboxValue(DWORD, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetComboBoxSelectedValue(DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CommandLinkClicked(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* response, CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serialization, PWSTR* text, CREDENTIAL_PROVIDER_STATUS_ICON* icon) override {
        if (!response || !serialization || !text || !icon) {
            return E_POINTER;
        }
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        *serialization = {};
        *text = nullptr;
        *icon = CPSI_NONE;
        auto buffer = attempt_->take();
        if (!buffer.data()) {
            return S_OK;
        }
        *serialization = buffer.release(kWindowsLoginProviderId);
        *response = CPGSR_RETURN_CREDENTIAL_FINISHED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReportResult(NTSTATUS status, NTSTATUS, PWSTR* text, CREDENTIAL_PROVIDER_STATUS_ICON* icon) override {
        if (!text || !icon) {
            return E_POINTER;
        }
        attempt_->cancel();
        *text = nullptr;
        *icon = status < 0 ? CPSI_ERROR : CPSI_NONE;
        return status < 0 ? copyText(L"自动登录未成功，请使用 Windows 登录选项手动登录。", text) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetUserSid(PWSTR* sid) override { return copyText(sid_, sid); }

  private:
    WindowsLoginModuleReference module_;
    std::atomic<ULONG> references_{1};
    std::wstring account_, sid_;
    std::shared_ptr<WindowsLoginAttempt> attempt_;
};
} // namespace
HRESULT createWindowsLoginCredential(WindowsLogonBuffer buffer, std::wstring account, std::wstring sid,
                                     std::shared_ptr<WindowsLoginAttempt> attempt, ICredentialProviderCredential2** credential) noexcept {
    if (!credential) {
        return E_POINTER;
    }
    *credential = nullptr;
    if (!buffer.data() || !buffer.size() || account.empty() || account.size() > 385 || account.find(L'\0') != std::wstring::npos ||
        sid.empty() || sid.size() > 184 || sid.find(L'\0') != std::wstring::npos || !attempt) {
        return E_INVALIDARG;
    }
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidW(sid.c_str(), &parsed)) {
        return E_INVALIDARG;
    }
    LocalFree(parsed);
    try {
        auto* created = new LoginCredential(std::move(account), std::move(sid), attempt);
        if (!attempt->bind(std::move(buffer))) {
            created->Release();
            return E_INVALIDARG;
        }
        *credential = created;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
} // namespace ultraterimal::agent
