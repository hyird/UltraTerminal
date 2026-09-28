#pragma once

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <cstddef>
#include <memory>
#include <span>

namespace ultraterimal::server {
class NativeStream {
  public:
    virtual ~NativeStream() = default;
    virtual asio::awaitable<void> handshake() = 0;
    virtual asio::awaitable<void> read(std::span<std::byte> bytes) = 0;
    virtual asio::awaitable<void> write(std::span<const std::byte> bytes) = 0;
    virtual void cancel() = 0;
    // Called after application I/O has joined; finishes transport shutdown.
    virtual asio::awaitable<void> close() = 0;
};
std::unique_ptr<NativeStream> makeTlsStream(asio::ip::tcp::socket socket, asio::ssl::context& context);
} // namespace ultraterimal::server
