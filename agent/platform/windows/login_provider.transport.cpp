#include "login_provider.transport.h"

#include <cstring>
#include <cwchar>
#include <new>
#include <utility>

namespace ultraterimal::agent {
namespace {
constexpr wchar_t kWindowClassPrefix[] = L"Ultraterimal.Login";
constexpr UINT_PTR kExpiryTimer = 1;
constexpr UINT kLoginMessage = WM_APP + 71;
std::wstring wideText(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string_view::npos) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}
} // namespace
WindowsLoginProvider::WindowsLoginProvider() noexcept : thread_(GetCurrentThreadId()) {}
WindowsLoginProvider::~WindowsLoginProvider() {
    clear();
    inbox_.reset();
    if (window_) {
        DestroyWindow(window_);
    }
    if (windowClass_) {
        UnregisterClassW(MAKEINTATOM(windowClass_), instance_);
    }
}
HRESULT WindowsLoginProvider::ensureWindow() noexcept {
    if (window_) {
        return S_OK;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, kWindowClassPrefix, &instance_)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    wchar_t name[96]{};
    swprintf_s(name, L"%s.%p", kWindowClassPrefix, static_cast<void*>(this));
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.hInstance = instance_;
    type.lpfnWndProc = windowProcedure;
    type.lpszClassName = name;
    windowClass_ = RegisterClassExW(&type);
    if (!windowClass_) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    window_ = CreateWindowExW(0, MAKEINTATOM(windowClass_), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, this);
    if (!window_) {
        const auto error = GetLastError();
        UnregisterClassW(MAKEINTATOM(windowClass_), instance_);
        windowClass_ = 0;
        return HRESULT_FROM_WIN32(error);
    }
    return S_OK;
}
bool WindowsLoginProvider::armExpiry() noexcept {
    const auto now = GetTickCount64();
    const auto remaining = expiresAt_ > now ? expiresAt_ - now : 1;
    return SetTimer(window_, kExpiryTimer, static_cast<UINT>(remaining), nullptr) != 0;
}
LRESULT CALLBACK WindowsLoginProvider::windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    auto* self = reinterpret_cast<WindowsLoginProvider*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == kLoginMessage && self && self->inbox_ && self->inbox_->matches(wparam)) {
        self->AddRef();
        self->receiveLogin();
        self->Release();
        return 0;
    }
    if (message == WM_TIMER && wparam == kExpiryTimer && self) {
        // CredentialsChanged may reenter and release the caller's COM reference.
        self->AddRef();
        if (self->attempt_ && (GetTickCount64() >= self->expiresAt_ || !self->armExpiry())) {
            (void)self->revoke();
        }
        self->Release();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool WindowsLoginProvider::ownerThread() const noexcept { return thread_ == GetCurrentThreadId(); }
void WindowsLoginProvider::clear() noexcept {
    if (window_) {
        KillTimer(window_, kExpiryTimer);
    }
    expiresAt_ = 0;
    if (attempt_) {
        attempt_->cancel();
    }
    credential_.Reset();
    attempt_.reset();
}
HRESULT WindowsLoginProvider::changed() noexcept {
    // Keep the callback alive across reentrant UnAdvise from LogonUI.
    const auto events = events_;
    return events ? events->CredentialsChanged(context_) : S_OK;
}
HRESULT WindowsLoginProvider::publish(WindowsLogonBuffer buffer, std::wstring account, std::wstring sid,
                                      CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, ULONGLONG expiresAt, UniqueHandle servicePipe) noexcept {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    const auto now = GetTickCount64();
    if ((scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) || scenario != scenario_ ||
        expiresAt <= now || expiresAt - now > 30000) {
        return E_INVALIDARG;
    }
    const auto windowStatus = ensureWindow();
    if (FAILED(windowStatus)) {
        return windowStatus;
    }
    try {
        auto attempt = std::make_shared<WindowsLoginAttempt>(expiresAt, std::move(servicePipe));
        Microsoft::WRL::ComPtr<ICredentialProviderCredential2> credential;
        const auto result = createWindowsLoginCredential(std::move(buffer), std::move(account), std::move(sid), attempt, &credential);
        if (FAILED(result)) {
            return result;
        }
        clear();
        attempt_ = std::move(attempt);
        credential_ = std::move(credential);
        expiresAt_ = expiresAt;
        if (!armExpiry()) {
            clear();
            return E_FAIL;
        }
        const auto notification = changed();
        if (FAILED(notification)) {
            clear();
        }
        return notification;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}
HRESULT WindowsLoginProvider::revoke() noexcept {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    const bool hadCredential = !!credential_;
    clear();
    return hadCredential ? changed() : S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::QueryInterface(REFIID id, void** output) {
    if (!output) {
        return E_POINTER;
    }
    *output = nullptr;
    if (id == __uuidof(ICredentialProviderSetUserArray)) {
        *output = static_cast<ICredentialProviderSetUserArray*>(this);
        AddRef();
        return S_OK;
    }
    if (id != __uuidof(IUnknown) && id != __uuidof(ICredentialProvider)) {
        return E_NOINTERFACE;
    }
    *output = static_cast<ICredentialProvider*>(this);
    AddRef();
    return S_OK;
}
ULONG STDMETHODCALLTYPE WindowsLoginProvider::AddRef() { return ++references_; }
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::SetUserArray(ICredentialProviderUserArray* users) {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    // Identity is resolved by the authorized service, not chosen by enumeration.
    // GetUserSid associates the delivered credential with its Windows user tile.
    return users ? S_OK : E_POINTER;
}
ULONG STDMETHODCALLTYPE WindowsLoginProvider::Release() {
    const auto remaining = --references_;
    if (!remaining) {
        delete this;
    }
    return remaining;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, DWORD) {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    clear();
    inbox_.reset();
    scenario_ = CPUS_INVALID;
    if (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) {
        return E_NOTIMPL;
    }
    scenario_ = scenario;
    startInbox();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION*) {
    // Only a verified Agent delivery may populate this provider.
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::Advise(ICredentialProviderEvents* events, UINT_PTR context) {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    if (!events) {
        return E_POINTER;
    }
    events_ = events;
    context_ = context;
    const auto status = ensureWindow();
    if (FAILED(status)) {
        return status;
    }
    startInbox();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::UnAdvise() {
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    clear();
    inbox_.reset();
    events_.Reset();
    context_ = 0;
    return S_OK;
}
void WindowsLoginProvider::startInbox() noexcept {
    if (!events_ || !window_ || inbox_ || (scenario_ != CPUS_LOGON && scenario_ != CPUS_UNLOCK_WORKSTATION)) {
        return;
    }
    auto inbox = WindowsLoginInbox::create(window_, kLoginMessage, scenario_);
    if (inbox) {
        inbox_ = std::move(*inbox);
    }
}
void WindowsLoginProvider::receiveLogin() noexcept {
    try {
        auto received = inbox_->receive();
        if (!received) {
            (void)revoke();
            inbox_.reset();
            startInbox();
            return;
        }
        if (!*received) {
            return;
        }
        auto& message = **received;
        struct Erase {
            wire::WindowsLoginDelivery& message;
            ~Erase() {
                auto* bytes = message.mutable_protected_logon();
                SecureZeroMemory(bytes->data(), bytes->size());
            }
        } erase{message};
        const auto scenario = message.scenario() == wire::WindowsLoginHello::LOGON    ? CPUS_LOGON
                              : message.scenario() == wire::WindowsLoginHello::UNLOCK ? CPUS_UNLOCK_WORKSTATION
                                                                                      : CPUS_INVALID;
        if (message.version() != 1 || scenario != scenario_) {
            return;
        }
        auto buffer = importWindowsLogon({reinterpret_cast<const BYTE*>(message.protected_logon().data()), message.protected_logon().size()},
                                         message.authentication_package(), scenario);
        auto lease = inbox_->lease();
        if (buffer && lease) {
            (void)publish(std::move(*buffer), wideText(message.account()), wideText(message.sid()), scenario, message.expires_at(), std::move(*lease));
        }
    } catch (...) {
        (void)revoke();
        inbox_.reset();
        startInbox();
    }
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::GetFieldDescriptorCount(DWORD* count) {
    if (!count) {
        return E_POINTER;
    }
    *count = 2;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::GetFieldDescriptorAt(DWORD index, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** descriptor) {
    if (!descriptor) {
        return E_POINTER;
    }
    *descriptor = nullptr;
    if (index > 1) {
        return E_INVALIDARG;
    }
    const std::wstring_view label = index == 0 ? L"Windows 账户" : L"自动登录";
    auto* field = static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
    if (!field) {
        return E_OUTOFMEMORY;
    }
    *field = {};
    field->dwFieldID = index;
    field->cpft = index == 0 ? CPFT_LARGE_TEXT : CPFT_SMALL_TEXT;
    field->pszLabel = static_cast<PWSTR>(CoTaskMemAlloc((label.size() + 1) * sizeof(wchar_t)));
    if (!field->pszLabel) {
        CoTaskMemFree(field);
        return E_OUTOFMEMORY;
    }
    std::memcpy(field->pszLabel, label.data(), label.size() * sizeof(wchar_t));
    field->pszLabel[label.size()] = L'\0';
    *descriptor = field;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::GetCredentialCount(DWORD* count, DWORD* selected, BOOL* automatic) {
    if (!count || !selected || !automatic) {
        return E_POINTER;
    }
    *count = 0;
    *selected = CREDENTIAL_PROVIDER_NO_DEFAULT;
    *automatic = FALSE;
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    if (!attempt_ || !attempt_->available()) {
        clear();
        return S_OK;
    }
    *count = 1;
    *selected = 0;
    *automatic = TRUE;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE WindowsLoginProvider::GetCredentialAt(DWORD index, ICredentialProviderCredential** credential) {
    if (!credential) {
        return E_POINTER;
    }
    *credential = nullptr;
    if (!ownerThread()) {
        return RPC_E_WRONG_THREAD;
    }
    if (index || !attempt_ || !attempt_->available()) {
        return E_INVALIDARG;
    }
    return credential_.CopyTo(credential);
}
} // namespace ultraterimal::agent
