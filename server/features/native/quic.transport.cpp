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

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ultraterimal/protocol/stream.protocol.h"

namespace ultraterimal::server {
struct QuicListener::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    asio::ip::udp::socket socket;
    asio::steady_timer timer;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> listener{nullptr, SSL_free};
    std::function<void(std::unique_ptr<NativeStream>)> accepted;
    std::set<asio::steady_timer*> waiters;
    std::set<SSL*> connections;
    std::string alpn;
    bool stopped = false;

    State(ruvia::event_loop owner, const ServerConfig& config, std::function<void(std::unique_ptr<NativeStream>)> accept)
        : loop(owner), socket(owner.executor()), timer(owner.executor()), accepted(std::move(accept)) {
        context.reset(SSL_CTX_new(OSSL_QUIC_server_method()));
        if (!context || SSL_CTX_use_certificate_chain_file(context.get(), config.nativeCertificateFile.string().c_str()) != 1 || SSL_CTX_use_PrivateKey_file(context.get(), config.nativeKeyFile.string().c_str(), SSL_FILETYPE_PEM) != 1 || SSL_CTX_check_private_key(context.get()) != 1) {
            throw std::runtime_error("Native QUIC server identity unavailable");
        }
        alpn.assign(1, static_cast<char>(sizeof(protocol::kNativeAlpn) - 1));
        alpn += protocol::kNativeAlpn;
        SSL_CTX_set_alpn_select_cb(context.get(), [](SSL*, const unsigned char** output, unsigned char* length, const unsigned char* input, unsigned inputLength, void* argument) -> int {
            const auto& protocols = static_cast<State*>(argument)->alpn;
            unsigned char* selected = nullptr;
            if (SSL_select_next_proto(&selected, length, reinterpret_cast<const unsigned char*>(protocols.data()), static_cast<unsigned>(protocols.size()), input, inputLength) != OPENSSL_NPN_NEGOTIATED) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            *output = selected;
            return SSL_TLSEXT_ERR_OK; }, this);
        const asio::ip::udp::endpoint endpoint(asio::ip::make_address(config.nativeAddress), config.nativePort);
        socket.open(endpoint.protocol());
        socket.bind(endpoint);
        socket.non_blocking(true);
        auto descriptor = socket.native_handle();
        if (descriptor > static_cast<decltype(descriptor)>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("QUIC socket descriptor overflow");
        }
        listener.reset(SSL_new_listener(context.get(), 0));
        if (!listener || SSL_set_fd(listener.get(), static_cast<int>(descriptor)) != 1 || SSL_set_blocking_mode(listener.get(), 0) != 1 || SSL_listen(listener.get()) != 1) {
            throw std::runtime_error("Native QUIC listen failed");
        }
    }
    void wake() {
        for (auto* waiter : waiters) {
            waiter->cancel();
        }
    }
    void stop() {
        stopped = true;
        timer.cancel();
        asio::error_code ignored;
        socket.cancel(ignored);
        socket.close(ignored);
        wake();
    }
    class Stream final : public NativeStream {
      public:
        Stream(std::shared_ptr<State> owner, SSL* connection)
            : owner_(std::move(owner)), connection_(connection, SSL_free), readWake_(owner_->loop.executor()), writeWake_(owner_->loop.executor()) {
            // Only the listener pump advances network state and then wakes all
            // pending operations. An implicit tick inside one stream's I/O can
            // consume events belonging to another sleeping operation.
            if (SSL_set_blocking_mode(connection_.get(), 0) != 1 ||
                SSL_set_event_handling_mode(connection_.get(), SSL_VALUE_EVENT_HANDLING_MODE_EXPLICIT) != 1) {
                throw std::runtime_error("Cannot configure nonblocking QUIC stream");
            }
            owner_->connections.insert(connection_.get());
        }
        ~Stream() override {
            abortConnection();
            owner_->connections.erase(connection_.get());
            owner_->timer.cancel();
        }
        asio::awaitable<void> handshake() override {
            while (!SSL_is_init_finished(connection_.get())) {
                requireOpen();
                ERR_clear_error();
                const int result = SSL_do_handshake(connection_.get());
                if (result != 1) {
                    requireRetry(result);
                    co_await wait(readWake_);
                }
            }
            const unsigned char* selected = nullptr;
            unsigned length = 0;
            SSL_get0_alpn_selected(connection_.get(), &selected, &length);
            if (length != sizeof(protocol::kNativeAlpn) - 1 || std::string_view(reinterpret_cast<const char*>(selected), length) != protocol::kNativeAlpn) {
                throw std::runtime_error("Native QUIC ALPN required");
            }
            co_return;
        }
        asio::awaitable<void> read(std::span<std::byte> bytes) override {
            while (!bytes.empty()) {
                requireOpen();
                ERR_clear_error();
                std::size_t received = 0;
                const int result = SSL_read_ex(connection_.get(), bytes.data(), bytes.size(), &received);
                if (result == 1 && received) {
                    bytes = bytes.subspan(received);
                    owner_->timer.cancel();
                } else {
                    requireRetry(result);
                    co_await wait(readWake_);
                }
            }
        }
        asio::awaitable<void> write(std::span<const std::byte> bytes) override {
            bool attempted = false;
            while (!bytes.empty()) {
                requireOpen();
                ERR_clear_error();
                std::size_t sent = 0;
                const int result = SSL_write_ex(connection_.get(), bytes.data(), bytes.size(), &sent);
                if (!attempted || (result == 1 && sent)) {
                    owner_->timer.cancel();
                }
                attempted = true;
                if (result == 1 && sent) {
                    bytes = bytes.subspan(sent);
                } else {
                    requireRetry(result);
                    co_await wait(writeWake_);
                }
            }
        }
        void cancel() override {
            if (closed_) {
                return;
            }
            closed_ = true;
            readWake_.cancel();
            writeWake_.cancel();
            owner_->timer.cancel();
        }
        asio::awaitable<void> close() override {
            cancel();
            // Keep the connection in the listener pump while OpenSSL flushes
            // admitted responses and retransmits its close notification.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            int previousShutdown = -1;
            while (!owner_->stopped && std::chrono::steady_clock::now() < deadline) {
                ERR_clear_error();
                const int result = SSL_shutdown_ex(connection_.get(), SSL_SHUTDOWN_FLAG_NO_BLOCK, nullptr, 0);
                if (result == 1) {
                    co_return;
                }
                if (result < 0) {
                    const auto reason = SSL_get_error(connection_.get(), result);
                    if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE) {
                        break;
                    }
                }
                const int shutdown = SSL_get_shutdown(connection_.get());
                if (shutdown != previousShutdown) {
                    previousShutdown = shutdown;
                    owner_->timer.cancel();
                }
                co_await wait(readWake_, deadline);
            }
            abortConnection();
        }

      private:
        void abortConnection() {
            if (owner_->stopped) {
                return;
            }
            (void)SSL_shutdown_ex(connection_.get(), SSL_SHUTDOWN_FLAG_NO_BLOCK | SSL_SHUTDOWN_FLAG_RAPID | SSL_SHUTDOWN_FLAG_NO_STREAM_FLUSH, nullptr, 0);
            (void)SSL_handle_events(connection_.get());
            // This tick can process other connections on the shared socket.
            owner_->wake();
        }
        void requireOpen() const {
            if (closed_ || owner_->stopped) {
                throw std::runtime_error("Native QUIC stream closed");
            }
        }
        void requireRetry(int result) {
            const int reason = SSL_get_error(connection_.get(), result);
            if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE) {
                throw std::runtime_error("Native QUIC stream failed");
            }
        }
        asio::awaitable<void> wait(asio::steady_timer& timer) {
            requireOpen();
            co_await wait(timer, std::chrono::steady_clock::time_point::max());
            requireOpen();
        }
        asio::awaitable<void> wait(asio::steady_timer& timer, std::chrono::steady_clock::time_point deadline) {
            timer.expires_at(deadline);
            owner_->waiters.insert(&timer);
            struct Registration {
                State& owner;
                asio::steady_timer* timer;
                ~Registration() { owner.waiters.erase(timer); }
            } registration{*owner_, &timer};
            asio::error_code ignored;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        std::shared_ptr<State> owner_;
        std::unique_ptr<SSL, decltype(&SSL_free)> connection_;
        asio::steady_timer readWake_, writeWake_;
        bool closed_ = false;
    };
    static asio::awaitable<void> run(std::shared_ptr<State> self) {
        try {
            while (!self->stopped) {
                ERR_clear_error();
                if (SSL_handle_events(self->listener.get()) != 1) {
                    throw std::runtime_error("Native QUIC listener failed");
                }
                while (auto* connection = SSL_accept_connection(self->listener.get(), SSL_ACCEPT_CONNECTION_NO_BLOCK)) {
                    self->accepted(std::make_unique<Stream>(self, connection));
                }
                for (auto* connection : self->connections) {
                    (void)SSL_handle_events(connection);
                }
                self->wake();
                auto deadline = std::chrono::steady_clock::time_point::max();
                bool writeDesired = false;
                const auto inspect = [&](SSL* connection) {
                    timeval timeout{};
                    int infinite = 0;
                    if (SSL_get_event_timeout(connection, &timeout, &infinite) == 1 && !infinite) {
                        deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(timeout.tv_sec) + std::chrono::microseconds(timeout.tv_usec));
                    }
                    writeDesired = writeDesired || SSL_net_write_desired(connection);
                };
                inspect(self->listener.get());
                for (auto* connection : self->connections) {
                    inspect(connection);
                }
                self->timer.expires_at(deadline);
                asio::error_code socketError, timerError;
                using namespace asio::experimental::awaitable_operators;
                const auto event = co_await (self->socket.async_wait(writeDesired ? asio::ip::udp::socket::wait_write : asio::ip::udp::socket::wait_read, asio::redirect_error(asio::use_awaitable, socketError)) || self->timer.async_wait(asio::redirect_error(asio::use_awaitable, timerError)));
                if (event.index() == 0 && socketError && !self->stopped) {
                    throw std::system_error(socketError);
                }
            }
        } catch (...) {
            self->stop();
            throw;
        }
    }
};
QuicListener::QuicListener(ruvia::event_loop loop, const ServerConfig& config, std::function<void(std::unique_ptr<NativeStream>)> accept) : state_(std::make_shared<State>(loop, config, std::move(accept))) {}
QuicListener::~QuicListener() { stop(); }
asio::awaitable<void> QuicListener::run() { return State::run(state_); }
void QuicListener::stop() { state_->stop(); }
} // namespace ultraterimal::server
