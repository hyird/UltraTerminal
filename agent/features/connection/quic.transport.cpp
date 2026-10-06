#include "quic.transport.h"

#include <algorithm>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/udp.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ultraterimal/protocol/stream.protocol.h"

namespace ultraterimal::agent {
struct QuicTransport::State : std::enable_shared_from_this<State> {
    using Clock = std::chrono::steady_clock;
    ConnectionConfig config;
    std::size_t maximum;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> connection{nullptr, SSL_free};
    asio::ip::udp::resolver resolver;
    asio::ip::udp::socket socket;
    asio::steady_timer connectDeadline;
    asio::steady_timer eventTimer;
    std::set<asio::steady_timer*> waiters;
    std::uint64_t eventGeneration = 0;
    bool started = false, connected = false, cancelled = false;
    bool reading = false, writing = false;

    State(ruvia::execution_handle loop, ConnectionConfig settings, std::size_t limit)
        : config(std::move(settings)), maximum(limit), resolver(loop.executor()), socket(loop.executor()), connectDeadline(loop.executor()), eventTimer(loop.executor()) {
        if (config.host.empty() || config.host.find('\0') != std::string::npos || maximum == 0 || maximum > protocol::kMaximumMessageBytes || config.connectTimeout <= std::chrono::seconds::zero()) {
            throw std::invalid_argument("Invalid QUIC configuration");
        }
        context.reset(SSL_CTX_new(OSSL_QUIC_client_method()));
        if (!context) {
            throw std::runtime_error("QUIC unavailable in the linked OpenSSL");
        }
        SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
        const int trusted = config.caFile.empty() ? SSL_CTX_set_default_verify_paths(context.get()) : SSL_CTX_load_verify_locations(context.get(), config.caFile.string().c_str(), nullptr);
        if (trusted != 1) {
            throw std::runtime_error("Cannot load QUIC trust store");
        }
    }
    void cancel() {
        if (!cancelled && connected && connection && socket.is_open()) {
            // Abort must reach connection closure even with unacknowledged
            // stream data. Orderly close() separately provides a drain deadline.
            (void)SSL_shutdown_ex(connection.get(), SSL_SHUTDOWN_FLAG_NO_BLOCK | SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_STREAM_FLUSH, nullptr, 0);
            (void)SSL_handle_events(connection.get());
        }
        cancelled = true;
        connected = false;
        resolver.cancel();
        connectDeadline.cancel();
        eventTimer.cancel();
        for (auto* waiter : waiters) {
            waiter->cancel();
        }
        asio::error_code ignored;
        socket.cancel(ignored);
        socket.close(ignored);
    }
    void scheduleEvents() {
        if (!connected || cancelled) {
            return;
        }
        const auto generation = ++eventGeneration;
        asio::error_code ignored;
        socket.cancel(ignored);
        eventTimer.cancel();
        timeval timeout{};
        int infinite = 0;
        if (SSL_get_event_timeout(connection.get(), &timeout, &infinite) != 1) {
            throw std::runtime_error("Cannot read QUIC event deadline");
        }
        eventTimer.expires_at(infinite ? Clock::time_point::max() : Clock::now() + std::chrono::seconds(timeout.tv_sec) + std::chrono::microseconds(timeout.tv_usec));
        const auto ready = [self = shared_from_this(), generation](const asio::error_code& error) {
            if (self->cancelled || generation != self->eventGeneration || error == asio::error::operation_aborted) {
                return;
            }
            if (error) {
                self->cancel();
                return;
            }
            ERR_clear_error();
            if (SSL_handle_events(self->connection.get()) != 1) {
                self->cancel();
                return;
            }
            for (auto* waiter : self->waiters) {
                waiter->cancel();
            }
            try {
                self->scheduleEvents();
            } catch (...) {
                self->cancel();
            }
        };
        socket.async_wait(SSL_net_write_desired(connection.get()) ? asio::ip::udp::socket::wait_write : asio::ip::udp::socket::wait_read, ready);
        eventTimer.async_wait(ready);
    }
    asio::awaitable<bool> wait(Clock::time_point deadline) {
        if (cancelled || Clock::now() >= deadline) {
            co_return false;
        }
        if (connected) {
            asio::steady_timer timer(socket.get_executor());
            timer.expires_at(deadline);
            waiters.insert(&timer);
            struct Registration {
                State& state;
                asio::steady_timer* timer;
                ~Registration() { state.waiters.erase(timer); }
            } registration{*this, &timer};
            asio::error_code ignored;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
            co_return !cancelled && Clock::now() < deadline;
        }
        timeval timeout{};
        int infinite = 0;
        if (SSL_get_event_timeout(connection.get(), &timeout, &infinite) != 1) {
            throw std::runtime_error("Cannot read QUIC event deadline");
        }
        auto wake = deadline;
        if (!infinite) {
            wake = std::min(wake, Clock::now() + std::chrono::seconds(timeout.tv_sec) + std::chrono::microseconds(timeout.tv_usec));
        }
        asio::steady_timer timer(socket.get_executor());
        timer.expires_at(wake);
        asio::error_code socketError, timerError;
        using namespace asio::experimental::awaitable_operators;
        const auto event = co_await (socket.async_wait(SSL_net_write_desired(connection.get()) ? asio::ip::udp::socket::wait_write : asio::ip::udp::socket::wait_read, asio::redirect_error(asio::use_awaitable, socketError)) || timer.async_wait(asio::redirect_error(asio::use_awaitable, timerError)));
        if (cancelled || (event.index() == 0 && socketError) || Clock::now() >= deadline) {
            co_return false;
        }
        ERR_clear_error();
        if (SSL_handle_events(connection.get()) != 1) {
            throw std::runtime_error("QUIC event processing failed");
        }
        co_return true;
    }
    static asio::awaitable<bool> connect(std::shared_ptr<State> self) {
        if (self->started || self->cancelled) {
            throw std::runtime_error("QUIC connection already used");
        }
        self->started = true;
        const auto deadline = Clock::now() + std::min(self->config.connectTimeout, std::chrono::seconds(2));
        self->connectDeadline.expires_at(deadline);
        self->connectDeadline.async_wait([self](const asio::error_code& error) {
            if (!error) {
                self->cancel();
            }
        });
        try {
            asio::error_code error;
            const auto endpoints = co_await self->resolver.async_resolve(self->config.address.empty() ? self->config.host : self->config.address, self->config.port, asio::redirect_error(asio::use_awaitable, error));
            if (error || self->cancelled || endpoints.empty()) {
                self->cancel();
                co_return false;
            }
            auto selected = endpoints.begin();
            for (auto candidate = endpoints.begin(); candidate != endpoints.end(); ++candidate) {
                if (candidate->endpoint().address().is_v4()) {
                    selected = candidate;
                    break;
                }
            }
            const auto endpoint = selected->endpoint();
            self->socket.open(endpoint.protocol());
            self->socket.connect(endpoint, error);
            if (error) {
                self->cancel();
                co_return false;
            }
            self->socket.non_blocking(true);
            const auto descriptor = self->socket.native_handle();
            if (descriptor > static_cast<decltype(descriptor)>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("QUIC socket descriptor overflow");
            }
            self->connection.reset(SSL_new(self->context.get()));
            if (!self->connection) {
                throw std::runtime_error("QUIC connection allocation failed");
            }
            auto* bio = BIO_new_dgram(static_cast<int>(descriptor), BIO_NOCLOSE);
            if (!bio) {
                throw std::runtime_error("QUIC socket allocation failed");
            }
            SSL_set_bio(self->connection.get(), bio, bio);
            std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)> address(BIO_ADDR_new(), BIO_ADDR_free);
            const auto v4 = endpoint.address().is_v4();
            const auto ipv4 = v4 ? endpoint.address().to_v4().to_bytes() : asio::ip::address_v4::bytes_type{};
            const auto ipv6 = v4 ? asio::ip::address_v6::bytes_type{} : endpoint.address().to_v6().to_bytes();
            if (!address || BIO_ADDR_rawmake(address.get(), v4 ? AF_INET : AF_INET6, v4 ? ipv4.data() : ipv6.data(), v4 ? ipv4.size() : ipv6.size(), htons(endpoint.port())) != 1 || SSL_set1_initial_peer_addr(self->connection.get(), address.get()) != 1 || SSL_set_blocking_mode(self->connection.get(), 0) != 1 || SSL_set1_host(self->connection.get(), self->config.host.c_str()) != 1 || SSL_set_tlsext_host_name(self->connection.get(), self->config.host.c_str()) != 1) {
                throw std::runtime_error("QUIC endpoint configuration failed");
            }
            std::string alpn(1, static_cast<char>(sizeof(protocol::kNativeAlpn) - 1));
            alpn += protocol::kNativeAlpn;
            if (SSL_set_alpn_protos(self->connection.get(), reinterpret_cast<const unsigned char*>(alpn.data()), static_cast<unsigned>(alpn.size())) != 0) {
                throw std::runtime_error("QUIC ALPN configuration failed");
            }
            for (;;) {
                if (self->cancelled || Clock::now() >= deadline) {
                    self->cancel();
                    co_return false;
                }
                ERR_clear_error();
                const int result = SSL_connect(self->connection.get());
                if (result == 1) {
                    break;
                }
                const int reason = SSL_get_error(self->connection.get(), result);
                if (SSL_get_verify_result(self->connection.get()) != X509_V_OK) {
                    throw std::runtime_error("QUIC certificate verification failed");
                }
                if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE) {
                    if (reason == SSL_ERROR_SYSCALL) {
                        self->cancel();
                        co_return false;
                    }
                    throw std::runtime_error("QUIC handshake rejected");
                }
                if (!(co_await self->wait(deadline))) {
                    self->cancel();
                    co_return false;
                }
            }
            self->connectDeadline.cancel();
            const unsigned char* selectedAlpn = nullptr;
            unsigned length = 0;
            SSL_get0_alpn_selected(self->connection.get(), &selectedAlpn, &length);
            if (length != sizeof(protocol::kNativeAlpn) - 1 || std::string_view(reinterpret_cast<const char*>(selectedAlpn), length) != protocol::kNativeAlpn) {
                throw std::runtime_error("QUIC ALPN mismatch");
            }
            self->connected = true;
            self->scheduleEvents();
            co_return true;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    asio::awaitable<void> transfer(char* data, std::size_t size, bool write, Clock::time_point deadline) {
        auto idleDeadline = deadline == Clock::time_point::max() ? deadline : std::min(deadline, Clock::now() + std::chrono::seconds(10));
        while (size) {
            if (!connected || cancelled) {
                throw std::runtime_error("QUIC connection closed");
            }
            if (Clock::now() >= deadline) {
                throw std::runtime_error("QUIC record transfer exceeded its deadline");
            }
            ERR_clear_error();
            std::size_t transferred = 0;
            const int result = write ? SSL_write_ex(connection.get(), data, size, &transferred) : SSL_read_ex(connection.get(), data, size, &transferred);
            const int reason = result == 1 ? SSL_ERROR_NONE : SSL_get_error(connection.get(), result);
            scheduleEvents();
            if (result == 1 && transferred > 0) {
                // Either API may process incoming datagrams for the other direction.
                for (auto* waiter : waiters) {
                    waiter->cancel();
                }
                data += transferred;
                size -= transferred;
                idleDeadline = std::min(deadline, Clock::now() + std::chrono::seconds(10));
                continue;
            }
            if ((reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE) || !(co_await wait(idleDeadline))) {
                throw std::runtime_error("QUIC stream closed or timed out");
            }
        }
    }
    static asio::awaitable<void> close(std::shared_ptr<State> self) {
        if (self->connected && !self->cancelled) {
            if (self->reading || self->writing) {
                throw std::logic_error("QUIC close requires completed application I/O");
            }
            // Keep the socket and protocol event pump alive during closing so
            // OpenSSL can retransmit CONNECTION_CLOSE. Cancellation remains
            // immediate; orderly close has a bounded lifetime even if offline.
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (!self->cancelled && Clock::now() < deadline) {
                ERR_clear_error();
                const int result = SSL_shutdown_ex(self->connection.get(), SSL_SHUTDOWN_FLAG_NO_BLOCK, nullptr, 0);
                if (result == 1) {
                    break;
                }
                if (result < 0) {
                    const int reason = SSL_get_error(self->connection.get(), result);
                    if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE) {
                        break;
                    }
                }
                self->scheduleEvents();
                if (!(co_await self->wait(deadline))) {
                    break;
                }
            }
        }
        self->cancel();
    }
    static asio::awaitable<void> send(std::shared_ptr<State> self, std::string bytes) {
        const auto header = protocol::encodeStreamHeader(bytes.size(), self->maximum);
        if (!self->connected || self->writing || !header) {
            throw std::runtime_error("Invalid QUIC write state or size");
        }
        self->writing = true;
        try {
            std::string record(reinterpret_cast<const char*>(header->data()), header->size());
            record += bytes;
            // A record may take longer than the idle-stall window on a lossy
            // path. Keep the operation bounded, but let steady retransmission
            // progress continue for the same assembly budget as downloads.
            co_await self->transfer(record.data(), record.size(), true, Clock::now() + std::chrono::seconds(60));
            self->writing = false;
        } catch (...) {
            self->writing = false;
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<std::string> receive(std::shared_ptr<State> self) {
        if (!self->connected || self->reading) {
            throw std::runtime_error("Invalid QUIC read state");
        }
        self->reading = true;
        try {
            protocol::StreamHeader header{};
            co_await self->transfer(reinterpret_cast<char*>(header.data()), header.size(), false, Clock::time_point::max());
            const auto length = protocol::decodeStreamHeader(header, self->maximum);
            if (!length) {
                throw std::runtime_error("Invalid QUIC record length");
            }
            std::string bytes(*length, '\0');
            // A lossy link may still make steady progress on a large record.
            // Bound stalls separately from total assembly time and byte limits.
            co_await self->transfer(bytes.data(), bytes.size(), false, Clock::now() + std::chrono::seconds(60));
            self->reading = false;
            co_return bytes;
        } catch (...) {
            self->reading = false;
            self->cancel();
            throw;
        }
    }
};
QuicTransport::QuicTransport(ruvia::execution_handle loop, const ConnectionConfig& config, std::size_t maximum) : state_(std::make_shared<State>(loop, config, maximum)) {}
QuicTransport::~QuicTransport() { state_->cancel(); }
asio::awaitable<bool> QuicTransport::connect() { return State::connect(state_); }
asio::awaitable<void> QuicTransport::send(std::string bytes) { return State::send(state_, std::move(bytes)); }
asio::awaitable<std::string> QuicTransport::receive() { return State::receive(state_); }
asio::awaitable<void> QuicTransport::close() { return State::close(state_); }
void QuicTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
