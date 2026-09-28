#include "tls.transport.h"

#include <algorithm>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include <openssl/ssl.h>

#include "ultraterimal/protocol/stream.protocol.h"

namespace ultraterimal::agent {
struct TlsTransport::State : std::enable_shared_from_this<State> {
    ConnectionConfig config;
    std::size_t maximum;
    asio::ssl::context context{asio::ssl::context::tls_client};
    asio::ip::tcp::resolver resolver;
    asio::ssl::stream<asio::ip::tcp::socket> stream;
    asio::steady_timer connectDeadline;
    asio::steady_timer writeDeadline;
    asio::steady_timer readDeadline;
    bool started = false;
    bool connected = false;
    bool cancelled = false;
    bool reading = false;
    bool writing = false;
    std::uint64_t readWaitGeneration = 0;

    State(ruvia::EventLoop loop, ConnectionConfig settings, std::size_t limit)
        : config(std::move(settings)), maximum(limit), resolver(loop.executor()), stream(loop.executor(), context), connectDeadline(loop.executor()), writeDeadline(loop.executor()), readDeadline(loop.executor()) {
        unsigned port = 0;
        const auto parsed = std::from_chars(config.port.data(), config.port.data() + config.port.size(), port);
        if (config.host.empty() || config.host.find('\0') != std::string::npos || parsed.ec != std::errc{} || parsed.ptr != config.port.data() + config.port.size() || !port || port > 65535 || config.connectTimeout <= std::chrono::seconds::zero() || maximum == 0 || maximum > protocol::kMaximumMessageBytes) {
            throw std::invalid_argument("Invalid native TLS configuration");
        }
        if (SSL_set_min_proto_version(stream.native_handle(), TLS1_3_VERSION) != 1) {
            throw std::runtime_error("Cannot require TLS 1.3");
        }
        // The context trust store is shared with its SSL instance.
        if (config.caFile.empty()) {
            context.set_default_verify_paths();
        } else {
            context.load_verify_file(config.caFile.string());
        }
        stream.set_verify_mode(asio::ssl::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification(config.host));
        if (SSL_set_tlsext_host_name(stream.native_handle(), config.host.c_str()) != 1) {
            throw std::runtime_error("Cannot set native TLS server name");
        }
        std::string alpn(1, static_cast<char>(sizeof(protocol::kNativeAlpn) - 1));
        alpn += protocol::kNativeAlpn;
        if (SSL_set_alpn_protos(stream.native_handle(), reinterpret_cast<const unsigned char*>(alpn.data()), static_cast<unsigned>(alpn.size())) != 0) {
            throw std::runtime_error("Cannot set native TLS ALPN");
        }
    }
    void cancel() {
        cancelled = true;
        connected = false;
        resolver.cancel();
        connectDeadline.cancel();
        writeDeadline.cancel();
        readDeadline.cancel();
        asio::error_code ignored;
        stream.lowest_layer().cancel(ignored);
        stream.lowest_layer().close(ignored);
    }
    void arm(asio::steady_timer& timer, std::chrono::seconds timeout) {
        timer.expires_after(timeout);
        timer.async_wait([self = shared_from_this()](const asio::error_code& error) {
            if (!error) {
                self->cancel();
            }
        });
    }
    static asio::awaitable<void> connect(std::shared_ptr<State> self) {
        if (self->started || self->cancelled) {
            throw std::runtime_error("Native TLS connection already used");
        }
        self->started = true;
        self->arm(self->connectDeadline, self->config.connectTimeout);
        try {
            const auto endpoints = co_await self->resolver.async_resolve(self->config.address.empty() ? self->config.host : self->config.address, self->config.port, asio::use_awaitable);
            co_await asio::async_connect(self->stream.next_layer(), endpoints, asio::use_awaitable);
            self->stream.next_layer().set_option(asio::ip::tcp::no_delay(true));
            co_await self->stream.async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
            const unsigned char* selected = nullptr;
            unsigned length = 0;
            SSL_get0_alpn_selected(self->stream.native_handle(), &selected, &length);
            if (length != sizeof(protocol::kNativeAlpn) - 1 || std::string_view(reinterpret_cast<const char*>(selected), length) != protocol::kNativeAlpn) {
                throw std::runtime_error("Native TLS ALPN mismatch");
            }
            if (self->cancelled) {
                throw std::runtime_error("Native TLS connect cancelled");
            }
            self->connected = true;
            self->connectDeadline.cancel();
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> send(std::shared_ptr<State> self, std::string bytes) {
        const auto header = protocol::encodeStreamHeader(bytes.size(), self->maximum);
        if (!self->connected || self->writing || !header) {
            throw std::runtime_error("Invalid native TLS write state or size");
        }
        self->writing = true;
        self->arm(self->writeDeadline, std::chrono::seconds(10));
        try {
            const std::array<asio::const_buffer, 2> buffers{asio::buffer(*header), asio::buffer(bytes)};
            co_await asio::async_write(self->stream, buffers, asio::use_awaitable);
            self->writeDeadline.cancel();
            self->writing = false;
        } catch (...) {
            self->writing = false;
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> readPart(std::shared_ptr<State> self, asio::mutable_buffer buffer, bool waitForRecord) {
        const auto deadline = waitForRecord ? std::chrono::steady_clock::time_point::max() : std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::size_t received = 0;
        while (received < buffer.size()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw asio::system_error(asio::error::operation_aborted);
            }
            const auto generation = ++self->readWaitGeneration;
            // An idle channel has no record deadline until its first header byte.
            if (!waitForRecord || received != 0) {
                self->readDeadline.expires_at(std::min(deadline, now + std::chrono::seconds(10)));
                self->readDeadline.async_wait([self, generation](const asio::error_code& error) {
                    if (!error && self->readWaitGeneration == generation) {
                        self->cancel();
                    }
                });
            }
            const auto count = co_await self->stream.async_read_some(buffer + received, asio::use_awaitable);
            ++self->readWaitGeneration;
            self->readDeadline.cancel();
            if (count == 0) {
                throw asio::system_error(asio::error::eof);
            }
            received += count;
        }
    }
    static asio::awaitable<std::string> receive(std::shared_ptr<State> self) {
        if (!self->connected || self->reading) {
            throw std::runtime_error("Invalid native TLS read state");
        }
        self->reading = true;
        try {
            protocol::StreamHeader header{};
            co_await readPart(self, asio::buffer(header), true);
            const auto length = protocol::decodeStreamHeader(header, self->maximum);
            if (!length) {
                throw std::runtime_error("Invalid native TLS record length");
            }
            std::string bytes(*length, '\0');
            co_await readPart(self, asio::buffer(bytes), false);
            self->readDeadline.cancel();
            self->reading = false;
            co_return bytes;
        } catch (...) {
            self->reading = false;
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> close(std::shared_ptr<State> self) {
        if (self->cancelled) {
            co_return;
        }
        // The owner must stop its reader/writer before orderly TLS shutdown.
        if (!self->connected || self->reading || self->writing) {
            self->cancel();
            co_return;
        }
        self->connected = false;
        self->arm(self->connectDeadline, self->config.connectTimeout);
        try {
            co_await self->stream.async_shutdown(asio::use_awaitable);
        } catch (...) {
            self->cancel();
            throw;
        }
        self->cancel();
    }
};
TlsTransport::TlsTransport(ruvia::EventLoop loop, const ConnectionConfig& config, std::size_t maximum) : state_(std::make_shared<State>(loop, config, maximum)) {}
TlsTransport::~TlsTransport() { state_->cancel(); }
asio::awaitable<void> TlsTransport::connect() { return State::connect(state_); }
asio::awaitable<void> TlsTransport::send(std::string bytes) { return State::send(state_, std::move(bytes)); }
asio::awaitable<std::string> TlsTransport::receive() { return State::receive(state_); }
asio::awaitable<void> TlsTransport::close() { return State::close(state_); }
void TlsTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
