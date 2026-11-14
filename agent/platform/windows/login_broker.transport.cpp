#include "login_broker.transport.h"

#include <array>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/windows/object_handle.hpp>
#include <asio/windows/stream_handle.hpp>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include <ruvia/core/event_loop.h>
#include <sddl.h>

#include "login_peer.transport.h"
#include "windows_login.transport.h"

namespace ultraterimal::agent {
namespace {
std::string utf8(std::wstring_view value) {
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error("Invalid login identity");
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) != count) {
        throw std::runtime_error("Invalid login identity");
    }
    return result;
}
struct ProtectedRecord {
    std::string bytes;
    ~ProtectedRecord() { SecureZeroMemory(bytes.data(), bytes.size()); }
};
} // namespace
struct WindowsLoginBroker::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    std::filesystem::path credentialFile;
    DWORD session;
    asio::windows::object_handle ready;
    asio::windows::stream_handle pipe;
    asio::steady_timer deadline;
    UniqueHandle peer;
    bool stopped = false, started = false;

    State(ruvia::event_loop owner, std::filesystem::path path, DWORD selected)
        : loop(owner), credentialFile(std::move(path)), session(selected), ready(owner.executor()), pipe(owner.executor()), deadline(owner.executor()) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
            throw std::runtime_error("Cannot protect login readiness event");
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        UniqueHandle event(CreateEventW(&security, FALSE, FALSE, windowsLoginEventName(session).c_str()));
        const auto error = GetLastError();
        LocalFree(descriptor);
        if (!event || error == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("Login readiness event is already owned");
        }
        ready.assign(event.get());
        event.release();
    }
    void stop() {
        stopped = true;
        std::error_code ignored;
        ready.cancel(ignored);
        ready.close(ignored);
        pipe.cancel(ignored);
        pipe.close(ignored);
        peer.reset();
        deadline.cancel();
    }
    void arm(std::chrono::milliseconds timeout) {
        deadline.expires_after(timeout);
        deadline.async_wait([weak = weak_from_this()](std::error_code error) {
            if (auto self = weak.lock(); self && !error) {
                self->stop();
            }
        });
    }
    static asio::awaitable<void> runOwned(std::shared_ptr<State> self) {
        if (self->started) {
            throw std::logic_error("Login broker can only run once");
        }
        self->started = true;
        try {
            while (!self->stopped) {
                UniqueHandle pipe(CreateFileW(windowsLoginPipeName(self->session).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
                if (!pipe) {
                    const auto error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
                        break;
                    }
                    co_await self->ready.async_wait(asio::use_awaitable);
                    continue;
                }
                ULONG peerId = 0, peerSession = 0;
                if (!GetNamedPipeServerProcessId(pipe.get(), &peerId) || !GetNamedPipeServerSessionId(pipe.get(), &peerSession) || peerSession != self->session) {
                    break;
                }
                auto verified = openWindowsLoginUiPeer(peerId, self->session);
                if (!verified) {
                    break;
                }
                self->peer = std::move(*verified);
                DWORD mode = PIPE_READMODE_MESSAGE;
                if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr)) {
                    break;
                }
                self->pipe.assign(pipe.get());
                pipe.release();
                self->arm(std::chrono::seconds(10));
                std::array<BYTE, 256> helloBytes{};
                const auto count = co_await self->pipe.async_read_some(asio::buffer(helloBytes), asio::use_awaitable);
                wire::WindowsLoginHello hello;
                if (!count || !hello.ParseFromArray(helloBytes.data(), static_cast<int>(count)) || hello.version() != 1 || hello.windows_session_id() != self->session) {
                    break;
                }
                const auto scenario = hello.scenario() == wire::WindowsLoginHello::LOGON    ? CPUS_LOGON
                                      : hello.scenario() == wire::WindowsLoginHello::UNLOCK ? CPUS_UNLOCK_WORKSTATION
                                                                                            : CPUS_INVALID;
                if (scenario == CPUS_INVALID) {
                    break;
                }
                ProtectedRecord record;
                {
                    auto preparation = ruvia::run_blocking(self->loop.worker(), [path = self->credentialFile, scenario] {
                        return prepareSavedWindowsLogon(path, scenario);
                    });
                    auto prepared = (co_await ruvia::as_awaitable(std::move(preparation))).take_value();
                    if (!prepared || self->stopped || WaitForSingleObject(self->peer.get(), 0) != WAIT_TIMEOUT) {
                        break;
                    }
                    wire::WindowsLoginDelivery delivery;
                    delivery.set_version(1);
                    delivery.set_expires_at(GetTickCount64() + 10000);
                    delivery.set_scenario(hello.scenario());
                    delivery.set_authentication_package(prepared->buffer.package());
                    delivery.set_protected_logon(prepared->buffer.data(), prepared->buffer.size());
                    delivery.set_account(utf8(prepared->account));
                    delivery.set_sid(utf8(prepared->sid));
                    record.bytes = delivery.SerializeAsString();
                    auto* bytes = delivery.mutable_protected_logon();
                    SecureZeroMemory(bytes->data(), bytes->size());
                }
                if (record.bytes.empty() || record.bytes.size() > 32768) {
                    break;
                }
                self->arm(std::chrono::seconds(10));
                const auto sent = co_await self->pipe.async_write_some(asio::buffer(record.bytes), asio::use_awaitable);
                if (sent != record.bytes.size()) {
                    break;
                }
                SecureZeroMemory(record.bytes.data(), record.bytes.size());
                record.bytes.clear();
                // Keep the authorization lease alive until LogonUI closes or the
                // deadline/session cancellation revokes it. Never submit twice.
                std::array<BYTE, 1> unexpected{};
                (void)co_await self->pipe.async_read_some(asio::buffer(unexpected), asio::use_awaitable);
                break;
            }
        } catch (...) {
            // Auto-login is optional; retain the remote view and standard tiles.
            // Passwords and peer-controlled error strings never enter logs.
        }
        self->stop();
    }
};
WindowsLoginBroker::WindowsLoginBroker(std::shared_ptr<State> state) : state_(std::move(state)) {}
WindowsLoginBroker::~WindowsLoginBroker() { stop(); }
std::unique_ptr<WindowsLoginBroker> WindowsLoginBroker::create(ruvia::event_loop loop, std::filesystem::path credentialFile, std::uint32_t session) noexcept {
    if (!session || session == 0xffffffffUL || !credentialFile.is_absolute() || !openWindowsLoginServicePeer(GetCurrentProcessId())) {
        return nullptr;
    }
    try {
        return std::unique_ptr<WindowsLoginBroker>(new WindowsLoginBroker(std::make_shared<State>(loop, std::move(credentialFile), session)));
    } catch (...) {
        return nullptr;
    }
}
asio::awaitable<void> WindowsLoginBroker::run() { return State::runOwned(state_); }
void WindowsLoginBroker::stop() { state_->stop(); }
} // namespace ultraterimal::agent
