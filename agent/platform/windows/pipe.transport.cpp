#include "pipe.transport.h"

#include <algorithm>
#include <array>
#include <asio/async_result.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/windows/overlapped_ptr.hpp>
#include <asio/windows/stream_handle.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>

#include "handle.types.h"

namespace ultraterimal::agent {
namespace {
constexpr std::size_t kMaximumBytes = 16 * 1024 * 1024;
constexpr wchar_t kPrefix[] = L"\\\\.\\pipe\\Ultraterimal-";
struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() { LocalFree(value); }
};
void require(BOOL success, const char* operation) {
    if (!success) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }
}
std::vector<BYTE> tokenInformation(TOKEN_INFORMATION_CLASS kind) {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "Open process token");
    UniqueHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), kind, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 1024 * 1024) {
        throw std::runtime_error("Invalid token information size");
    }
    std::vector<BYTE> buffer(size);
    require(GetTokenInformation(token.get(), kind, buffer.data(), size, &size), "Read process token");
    return buffer;
}
std::wstring sidText(PSID sid) {
    LocalMemory text;
    require(ConvertSidToStringSidW(sid, reinterpret_cast<LPWSTR*>(&text.value)), "Encode SID");
    return static_cast<wchar_t*>(text.value);
}
struct OperationGuard {
    bool& flag;
    explicit OperationGuard(bool& active) : flag(active) {
        if (flag) {
            throw std::logic_error("Overlapping pipe operation");
        }
        flag = true;
    }
    ~OperationGuard() { flag = false; }
};
} // namespace
std::wstring currentLogonSid() {
    const auto buffer = tokenInformation(TokenGroups);
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer.data());
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            return sidText(groups->Groups[i].Sid);
        }
    }
    throw std::runtime_error("Process has no Windows logon SID");
}
struct NamedPipeTransport::State : std::enable_shared_from_this<State> {
    asio::windows::stream_handle pipe;
    asio::steady_timer acceptDeadline, readDeadline, writeDeadline;
    std::wstring name;
    bool server = false, accepting = false, reading = false, writing = false, ready = false;
    explicit State(asio::any_io_executor executor) : pipe(executor), acceptDeadline(executor), readDeadline(executor), writeDeadline(executor) {}
    void cancel() {
        ready = false;
        std::error_code ignored;
        pipe.cancel(ignored);
        pipe.close(ignored);
        acceptDeadline.cancel();
        readDeadline.cancel();
        writeDeadline.cancel();
    }
    void arm(asio::steady_timer& timer) {
        timer.expires_after(std::chrono::seconds(10));
        timer.async_wait([weak = weak_from_this()](std::error_code error) {
            if (auto state = weak.lock(); state && !error) {
                state->cancel();
            }
        });
    }
    static asio::awaitable<void> acceptOwned(std::shared_ptr<State> self, std::uint32_t process, std::uint32_t session) {
        OperationGuard guard(self->accepting);
        try {
            if (!process || !self->server || self->ready || !self->pipe.is_open()) {
                throw std::logic_error("Invalid pipe accept state");
            }
            self->arm(self->acceptDeadline);
            (void)co_await asio::async_initiate<decltype(asio::use_awaitable), void(std::error_code, std::size_t)>(
                [self](auto handler) {
                    asio::windows::overlapped_ptr overlapped(self->pipe.get_executor(), std::move(handler));
                    if (ConnectNamedPipe(self->pipe.native_handle(), overlapped.get())) {
                        overlapped.release();
                    } else {
                        const auto error = GetLastError();
                        if (error == ERROR_IO_PENDING) {
                            overlapped.release();
                        } else {
                            overlapped.complete(error == ERROR_PIPE_CONNECTED ? std::error_code{} : std::error_code(static_cast<int>(error), std::system_category()), 0);
                        }
                    }
                },
                asio::use_awaitable);
            self->acceptDeadline.cancel();
            ULONG peerProcess = 0, peerSession = 0;
            require(GetNamedPipeClientProcessId(self->pipe.native_handle(), &peerProcess), "Get pipe client process");
            require(GetNamedPipeClientSessionId(self->pipe.native_handle(), &peerSession), "Get pipe client session");
            if (peerProcess != process || peerSession != session) {
                throw std::runtime_error("Pipe client identity mismatch");
            }
            self->ready = true;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> sendOwned(std::shared_ptr<State> self, std::string message) {
        OperationGuard guard(self->writing);
        try {
            if (!self->ready || message.empty() || message.size() > kMaximumBytes) {
                throw std::runtime_error("Invalid pipe message or state");
            }
            const auto size = static_cast<std::uint32_t>(message.size());
            std::array<unsigned char, 4> length{};
            for (unsigned i = 0; i < 4; ++i) {
                length[i] = static_cast<unsigned char>(size >> (i * 8));
            }
            self->arm(self->writeDeadline);
            const std::array<asio::const_buffer, 2> buffers{asio::buffer(length), asio::buffer(message)};
            co_await asio::async_write(self->pipe, buffers, asio::use_awaitable);
            self->writeDeadline.cancel();
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<std::string> receiveOwned(std::shared_ptr<State> self) {
        OperationGuard guard(self->reading);
        try {
            if (!self->ready) {
                throw std::runtime_error("Pipe is not connected");
            }
            std::array<unsigned char, 4> length{};
            const auto headerBytes = co_await self->pipe.async_read_some(asio::buffer(length), asio::use_awaitable);
            if (headerBytes == 0) {
                throw std::runtime_error("Empty pipe frame header");
            }
            self->arm(self->readDeadline);
            if (headerBytes < length.size()) {
                co_await asio::async_read(self->pipe, asio::buffer(length.data() + headerBytes, length.size() - headerBytes), asio::use_awaitable);
            }
            std::uint32_t size = 0;
            for (unsigned i = 0; i < 4; ++i) {
                size |= static_cast<std::uint32_t>(length[i]) << (i * 8);
            }
            if (!size || size > kMaximumBytes) {
                throw std::runtime_error("Invalid pipe message length");
            }
            std::string message(size, '\0');
            co_await asio::async_read(self->pipe, asio::buffer(message), asio::use_awaitable);
            self->readDeadline.cancel();
            co_return message;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
};
NamedPipeTransport::NamedPipeTransport(asio::any_io_executor executor, const std::wstring& peerLogonSid) : state_(std::make_shared<State>(executor)) {
    LocalMemory peer;
    require(ConvertStringSidToSidW(peerLogonSid.c_str(), &peer.value), "Decode peer SID");
    const auto user = tokenInformation(TokenUser);
    const auto creator = sidText(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid);
    const bool systemPeer = peerLogonSid == L"S-1-5-18" && creator == L"S-1-5-18";
    if (!IsValidSid(peer.value) || (!peerLogonSid.starts_with(L"S-1-5-5-") && !systemPeer) || sidText(peer.value) != peerLogonSid) {
        throw std::invalid_argument("A canonical logon SID or service-owned SYSTEM peer is required");
    }
    const auto sddl = L"D:P(A;;GA;;;" + creator + L")(A;;0x00100003;;;" + peerLogonSid + L")";
    LocalMemory security;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, reinterpret_cast<PSECURITY_DESCRIPTOR*>(&security.value), nullptr), "Create pipe security");
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        throw std::runtime_error("Cannot create pipe identity");
    }
    state_->name = kPrefix;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto byte : random) {
        state_->name += digits[byte >> 4];
        state_->name += digits[byte & 15];
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.value, FALSE};
    UniqueHandle handle(CreateNamedPipeW(state_->name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes));
    require(bool(handle), "Create local desktop pipe");
    state_->pipe.assign(handle.get());
    handle.release();
    state_->server = true;
}
NamedPipeTransport::NamedPipeTransport(std::shared_ptr<State> state) : state_(std::move(state)) {}
NamedPipeTransport::~NamedPipeTransport() { state_->cancel(); }
const std::wstring& NamedPipeTransport::name() const { return state_->name; }
std::unique_ptr<NamedPipeTransport> NamedPipeTransport::connect(asio::any_io_executor executor, const std::wstring& name, std::uint32_t expectedServerProcess) {
    const auto prefixLength = std::size(kPrefix) - 1;
    if (!expectedServerProcess || !name.starts_with(kPrefix) || name.size() != prefixLength + 32 ||
        !std::all_of(name.begin() + prefixLength, name.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); })) {
        throw std::invalid_argument("Invalid desktop pipe endpoint");
    }
    auto state = std::make_shared<State>(executor);
    UniqueHandle handle(CreateFileW(name.c_str(), FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    require(bool(handle), "Open desktop pipe");
    ULONG process = 0;
    require(GetNamedPipeServerProcessId(handle.get(), &process), "Get pipe server process");
    if (process != expectedServerProcess) {
        throw std::runtime_error("Pipe server identity mismatch");
    }
    state->pipe.assign(handle.get());
    handle.release();
    state->name = name;
    state->ready = true;
    return std::unique_ptr<NamedPipeTransport>(new NamedPipeTransport(std::move(state)));
}
asio::awaitable<void> NamedPipeTransport::accept(std::uint32_t process, std::uint32_t session) { return State::acceptOwned(state_, process, session); }
asio::awaitable<void> NamedPipeTransport::send(std::string message) { return State::sendOwned(state_, std::move(message)); }
asio::awaitable<std::string> NamedPipeTransport::receive() { return State::receiveOwned(state_); }
void NamedPipeTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
