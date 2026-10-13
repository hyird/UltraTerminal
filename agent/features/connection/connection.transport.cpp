#include "connection.transport.h"

#include <asio/steady_timer.hpp>
#include <stdexcept>
#include <utility>

#include "native.transport.h"

namespace ultraterimal::agent {
struct DeviceTransport::State : std::enable_shared_from_this<State> {
    std::unique_ptr<NativeTransport> native;
    ConnectionConfig config;
    wire::NativeConnect::Purpose purpose;
    asio::steady_timer deadline;
    bool started = false, connected = false, cancelled = false;
    State(ruvia::event_loop loop, const ConnectionConfig& settings, std::size_t maximum, wire::NativeConnect::Purpose use)
        : config(settings), purpose(use), deadline(loop.executor()) {
        native = std::make_unique<NativeTransport>(loop, config, maximum);
    }
    void cancel() {
        cancelled = true;
        connected = false;
        deadline.cancel();
        if (native) {
            native->cancel();
        }
    }
    static asio::awaitable<void> connect(std::shared_ptr<State> self, std::string credential) {
        if (self->started || self->cancelled) {
            throw std::runtime_error("Device connection already used");
        }
        self->started = true;
        try {
            self->deadline.expires_after(self->config.connectTimeout);
            self->deadline.async_wait([self](const asio::error_code& error) {
                if (!error) {
                    self->cancel();
                }
            });
            wire::NativeConnect request;
            request.set_protocol_version(protocol::kVersion);
            request.set_purpose(self->purpose);
            request.set_credential(std::move(credential));
            const auto bytes = request.SerializeAsString();
            if (!protocol::decodeNativeConnect(bytes)) {
                throw std::runtime_error("Invalid native device authentication");
            }
            co_await self->native->connect();
            co_await self->native->send(bytes);
            request.Clear();
            auto response = protocol::decodeNativeAccepted(co_await self->native->receive(), false);
            if (!response || !response->accepted() || response->device_id() != self->config.deviceId) {
                throw std::runtime_error("Native device authentication rejected");
            }
            self->deadline.cancel();
            if (self->cancelled) {
                throw std::runtime_error("Device connection cancelled");
            }
            self->connected = true;
        } catch (...) {
            self->cancel();
            throw;
        }
    }
    static asio::awaitable<void> send(std::shared_ptr<State> self, std::string bytes) {
        if (!self->connected) {
            throw std::runtime_error("Device connection is not authenticated");
        }
        co_await self->native->send(std::move(bytes));
    }
    static asio::awaitable<std::string> receive(std::shared_ptr<State> self) {
        if (!self->connected) {
            throw std::runtime_error("Device connection is not authenticated");
        }
        co_return co_await self->native->receive();
    }
    static asio::awaitable<void> close(std::shared_ptr<State> self) {
        self->connected = false;
        co_await self->native->close();
        self->cancel();
        co_return;
    }
};
DeviceTransport::DeviceTransport(ruvia::event_loop loop, const ConnectionConfig& config, std::size_t maximum, wire::NativeConnect::Purpose purpose) : state_(std::make_shared<State>(loop, config, maximum, purpose)) {}
DeviceTransport::~DeviceTransport() { state_->cancel(); }
asio::awaitable<void> DeviceTransport::connect(std::string credential) { return State::connect(state_, std::move(credential)); }
asio::awaitable<void> DeviceTransport::send(std::string bytes) { return State::send(state_, std::move(bytes)); }
asio::awaitable<std::string> DeviceTransport::receive() { return State::receive(state_); }
asio::awaitable<void> DeviceTransport::close() { return State::close(state_); }
void DeviceTransport::cancel() { state_->cancel(); }
} // namespace ultraterimal::agent
