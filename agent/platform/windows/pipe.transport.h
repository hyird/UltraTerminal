#pragma once
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace ultraterimal::agent {
// Logon SID scopes access to a Windows logon, rather than all sessions of a user.
std::wstring currentLogonSid();
class NamedPipeTransport {
  public:
    // Creates one local-only instance with a fresh random name and a protected DACL.
    // Peer is a logon SID, or SYSTEM only when the creating service is SYSTEM.
    NamedPipeTransport(asio::any_io_executor executor, const std::wstring& peerLogonSid);
    ~NamedPipeTransport();
    NamedPipeTransport(const NamedPipeTransport&) = delete;
    NamedPipeTransport& operator=(const NamedPipeTransport&) = delete;
    const std::wstring& name() const;
    // The process owner keeps its peer process handle alive to prevent PID reuse.
    asio::awaitable<void> accept(std::uint32_t expectedProcess, std::uint32_t expectedSession);
    static std::unique_ptr<NamedPipeTransport> connect(asio::any_io_executor executor, const std::wstring& name, std::uint32_t expectedServerProcess);
    // Four-byte little-endian length, then 1..16 MiB payload. One reader and writer;
    // await each write for backpressure. All access and cancellation use one executor.
    asio::awaitable<void> send(std::string message);
    asio::awaitable<std::string> receive();
    void cancel();

  private:
    struct State;
    explicit NamedPipeTransport(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
