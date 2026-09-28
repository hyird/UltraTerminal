#include "login_inbox.transport.h"

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>

#include <sddl.h>

#include "login_peer.transport.h"

namespace ultraterimal::agent {
namespace {
std::atomic<UINT_PTR> nextCookie{0};
constexpr DWORD kMaximumDelivery = 32768;
} // namespace
struct WindowsLoginInbox::State {
    enum class Phase { kAccept,
                       kHello,
                       kDelivery,
                       kMonitor };
    HWND window;
    UINT message;
    const UINT_PTR cookie = ++nextCookie;
    DWORD session;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario;
    UniqueHandle pipe, event, peer;
    PTP_WAIT wait = nullptr;
    OVERLAPPED operation{};
    bool pending = false;
    Phase phase = Phase::kAccept;
    std::array<BYTE, kMaximumDelivery> input{};
    std::string output;

    State(HWND target, UINT notification, DWORD selected, CREDENTIAL_PROVIDER_USAGE_SCENARIO usage)
        : window(target), message(notification), session(selected), scenario(usage) {}
    ~State() {
        if (wait) {
            SetThreadpoolWait(wait, nullptr, nullptr);
            WaitForThreadpoolWaitCallbacks(wait, TRUE);
            CloseThreadpoolWait(wait);
        }
        if (pending) {
            CancelIoEx(pipe.get(), &operation);
            DWORD count = 0;
            GetOverlappedResult(pipe.get(), &operation, &count, TRUE);
        }
        SecureZeroMemory(input.data(), input.size());
    }
    void arm() {
        pending = true;
        SetThreadpoolWait(wait, event.get(), nullptr);
    }
    void prepare() {
        ResetEvent(event.get());
        operation = {};
        operation.hEvent = event.get();
    }
    HRESULT read(Phase next) {
        phase = next;
        prepare();
        const BOOL result = ReadFile(pipe.get(), input.data(), static_cast<DWORD>(input.size()), nullptr, &operation);
        if (!result && GetLastError() != ERROR_IO_PENDING) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        arm();
        return S_OK;
    }
    HRESULT start() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        pipe.reset(CreateNamedPipeW(windowsLoginPipeName(session).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    kMaximumDelivery, kMaximumDelivery, 0, &security));
        const auto pipeError = GetLastError();
        LocalFree(descriptor);
        if (!pipe) {
            return HRESULT_FROM_WIN32(pipeError);
        }
        event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        wait = CreateThreadpoolWait([](PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
            const auto& state = *static_cast<State*>(context);
            PostMessageW(state.window, state.message, state.cookie, 0);
        },
                                    this, nullptr);
        if (!wait) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        prepare();
        if (!ConnectNamedPipe(pipe.get(), &operation)) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                SetEvent(event.get());
            } else if (error != ERROR_IO_PENDING) {
                return HRESULT_FROM_WIN32(error);
            }
        }
        arm();
        // A paired writable session may be waiting for this LogonUI instance.
        // An event conveys availability only; both pipe peers still verify PID/token.
        UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, windowsLoginEventName(session).c_str()));
        if (ready) {
            SetEvent(ready.get());
        }
        return S_OK;
    }
    std::expected<std::optional<wire::WindowsLoginDelivery>, HRESULT> receive() {
        DWORD count = 0;
        if (!pending) {
            return std::optional<wire::WindowsLoginDelivery>{};
        }
        if (!GetOverlappedResult(pipe.get(), &operation, &count, FALSE)) {
            const auto error = GetLastError();
            if (error == ERROR_IO_INCOMPLETE) {
                return std::optional<wire::WindowsLoginDelivery>{};
            }
            pending = false;
            return std::unexpected(HRESULT_FROM_WIN32(error));
        }
        pending = false;
        if (phase == Phase::kAccept) {
            ULONG processId = 0, sessionId = 0;
            if (!GetNamedPipeClientProcessId(pipe.get(), &processId) || !GetNamedPipeClientSessionId(pipe.get(), &sessionId) || sessionId != 0) {
                return std::unexpected(E_ACCESSDENIED);
            }
            auto verified = openWindowsLoginServicePeer(processId);
            if (!verified) {
                return std::unexpected(E_ACCESSDENIED);
            }
            peer = std::move(*verified);
            wire::WindowsLoginHello hello;
            hello.set_version(1);
            hello.set_windows_session_id(session);
            hello.set_scenario(scenario == CPUS_LOGON ? wire::WindowsLoginHello::LOGON : wire::WindowsLoginHello::UNLOCK);
            output = hello.SerializeAsString();
            phase = Phase::kHello;
            prepare();
            if (!WriteFile(pipe.get(), output.data(), static_cast<DWORD>(output.size()), nullptr, &operation) && GetLastError() != ERROR_IO_PENDING) {
                return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
            }
            arm();
        } else if (phase == Phase::kHello) {
            if (count != output.size()) {
                return std::unexpected(E_FAIL);
            }
            output.clear();
            const auto result = read(Phase::kDelivery);
            if (FAILED(result)) {
                return std::unexpected(result);
            }
        } else if (phase == Phase::kDelivery) {
            wire::WindowsLoginDelivery delivery;
            const bool parsed = count && delivery.ParseFromArray(input.data(), static_cast<int>(count));
            SecureZeroMemory(input.data(), input.size());
            if (!parsed || WaitForSingleObject(peer.get(), 0) != WAIT_TIMEOUT) {
                return std::unexpected(E_ACCESSDENIED);
            }
            const auto result = read(Phase::kMonitor);
            if (FAILED(result)) {
                return std::unexpected(result);
            }
            return std::optional<wire::WindowsLoginDelivery>(std::move(delivery));
        } else {
            // No second submission or additional command is valid on this lease.
            return std::unexpected(E_ABORT);
        }
        return std::optional<wire::WindowsLoginDelivery>{};
    }
};
WindowsLoginInbox::WindowsLoginInbox(std::unique_ptr<State> state) : state_(std::move(state)) {}
WindowsLoginInbox::~WindowsLoginInbox() = default;
std::expected<std::unique_ptr<WindowsLoginInbox>, HRESULT> WindowsLoginInbox::create(HWND window, UINT message, CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) noexcept {
    DWORD session = 0;
    if (!window || (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &session) || !openWindowsLoginUiPeer(GetCurrentProcessId(), session)) {
        return std::unexpected(E_ACCESSDENIED);
    }
    try {
        auto state = std::make_unique<State>(window, message, session, scenario);
        const auto status = state->start();
        if (FAILED(status)) {
            return std::unexpected(status);
        }
        return std::unique_ptr<WindowsLoginInbox>(new WindowsLoginInbox(std::move(state)));
    } catch (...) {
        return std::unexpected(E_OUTOFMEMORY);
    }
}
bool WindowsLoginInbox::matches(WPARAM cookie) const noexcept { return cookie == state_->cookie; }
std::expected<std::optional<wire::WindowsLoginDelivery>, HRESULT> WindowsLoginInbox::receive() noexcept {
    try {
        return state_->receive();
    } catch (...) {
        return std::unexpected(E_FAIL);
    }
}
std::expected<UniqueHandle, HRESULT> WindowsLoginInbox::lease() const noexcept {
    if (state_->phase != State::Phase::kMonitor || WaitForSingleObject(state_->peer.get(), 0) != WAIT_TIMEOUT) {
        return std::unexpected(E_ACCESSDENIED);
    }
    HANDLE handle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), state_->pipe.get(), GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return std::unexpected(HRESULT_FROM_WIN32(GetLastError()));
    }
    return UniqueHandle(handle);
}
} // namespace ultraterimal::agent
