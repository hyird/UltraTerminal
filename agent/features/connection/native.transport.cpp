#include "native.transport.h"

#include <stdexcept>
#include <utility>

namespace ultraterimal::agent {
struct NativeTransport::State {
    QuicTransport quic;
    TlsTransport tls;
    NativeLink link = NativeLink::kDisconnected;
    bool started = false, cancelled = false;
    State(ruvia::execution_handle loop, const ConnectionConfig& config, std::size_t maximum) : quic(loop, config, maximum), tls(loop, config, maximum) {}
    void cancel() {
        cancelled = true;
        link = NativeLink::kDisconnected;
        quic.cancel();
        tls.cancel();
    }
    static asio::awaitable<void> connect(std::shared_ptr<State> self) {
        if (self->started || self->cancelled) {
            throw std::runtime_error("Native connection already used");
        }
        self->started = true;
        try {
            if (co_await self->quic.connect()) {
                if (self->cancelled) {
                    throw std::runtime_error("Native connection cancelled");
                }
                self->link = NativeLink::kQuic;
                co_return;
            }
            if (self->cancelled) {
                throw std::runtime_error("Native connection cancelled");
            }
            co_await self->tls.connect();
            if (self->cancelled) {
                throw std::runtime_error("Native connection cancelled");
            }
            self->link = NativeLink::kTls;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> close(std::shared_ptr<State> self) {
        try {
            if (self->link == NativeLink::kQuic) {
                co_await self->quic.close();
            } else if (self->link == NativeLink::kTls) {
                co_await self->tls.close();
            }
        } catch (...) {
            self->cancel();
            throw;
        }
        self->cancel();
    }
    static asio::awaitable<void> send(std::shared_ptr<State> self, std::string bytes) {
        try {
            if (self->link == NativeLink::kQuic) {
                co_await self->quic.send(std::move(bytes));
            } else if (self->link == NativeLink::kTls) {
                co_await self->tls.send(std::move(bytes));
            } else {
                throw std::runtime_error("Native connection is not ready");
            }
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<std::string> receive(std::shared_ptr<State> self) {
        try {
            if (self->link == NativeLink::kQuic) {
                co_return co_await self->quic.receive();
            }
            if (self->link == NativeLink::kTls) {
                co_return co_await self->tls.receive();
            }
            throw std::runtime_error("Native connection is not ready");
        } catch (...) {
            self->cancel();
            throw;
        }
    }
};
NativeTransport::NativeTransport(ruvia::execution_handle loop, const ConnectionConfig& config, std::size_t maximum) : state_(std::make_shared<State>(loop, config, maximum)) {}
NativeTransport::~NativeTransport() { state_->cancel(); }
asio::awaitable<void> NativeTransport::connect() { return State::connect(state_); }
asio::awaitable<void> NativeTransport::send(std::string bytes) { return State::send(state_, std::move(bytes)); }
asio::awaitable<std::string> NativeTransport::receive() { return State::receive(state_); }
asio::awaitable<void> NativeTransport::close() { return State::close(state_); }
NativeLink NativeTransport::link() const { return state_->link; }
void NativeTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
