#include "stream.transport.h"

#include <asio/read.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <stdexcept>

#include <openssl/ssl.h>

#include "ultraterimal/protocol/stream.protocol.h"

namespace ultraterimal::server {
namespace {
class TlsStream final : public NativeStream {
  public:
    TlsStream(asio::ip::tcp::socket socket, asio::ssl::context& context) : socket_(std::move(socket), context) {}
    asio::awaitable<void> handshake() override {
        socket_.next_layer().set_option(asio::ip::tcp::no_delay(true));
        co_await socket_.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        const unsigned char* selected = nullptr;
        unsigned length = 0;
        SSL_get0_alpn_selected(socket_.native_handle(), &selected, &length);
        if (length != sizeof(protocol::kNativeAlpn) - 1 || std::string_view(reinterpret_cast<const char*>(selected), length) != protocol::kNativeAlpn) {
            throw std::runtime_error("Native TLS ALPN required");
        }
    }
    asio::awaitable<void> read(std::span<std::byte> bytes) override {
        co_await asio::async_read(socket_, asio::buffer(bytes.data(), bytes.size()), asio::use_awaitable);
    }
    asio::awaitable<void> write(std::span<const std::byte> bytes) override {
        co_await asio::async_write(socket_, asio::buffer(bytes.data(), bytes.size()), asio::use_awaitable);
    }
    void cancel() override {
        asio::error_code ignored;
        socket_.lowest_layer().cancel(ignored);
        socket_.lowest_layer().close(ignored);
    }
    asio::awaitable<void> close() override { co_return; }

  private:
    asio::ssl::stream<asio::ip::tcp::socket> socket_;
};
} // namespace
std::unique_ptr<NativeStream> makeTlsStream(asio::ip::tcp::socket socket, asio::ssl::context& context) { return std::make_unique<TlsStream>(std::move(socket), context); }
} // namespace ultraterimal::server
