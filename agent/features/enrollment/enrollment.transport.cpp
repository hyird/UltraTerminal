#include "enrollment.transport.h"

#include <asio/steady_timer.hpp>
#include <memory>
#include <stdexcept>
#include <utility>

#include "features/connection/native.transport.h"
#include "ultraterimal/protocol/native.protocol.h"

namespace ultraterimal::agent {
asio::awaitable<EnrollmentIdentity> exchangeEnrollment(ruvia::event_loop loop, ConnectionConfig config, std::string code, std::string name) {
    wire::NativeConnect request;
    request.set_protocol_version(protocol::kVersion);
    request.set_purpose(wire::NativeConnect::ENROLLMENT);
    request.set_enrollment_code(std::move(code));
    request.set_device_name(std::move(name));
    const auto bytes = request.SerializeAsString();
    if (!protocol::decodeNativeConnect(bytes)) {
        throw std::runtime_error("Invalid native enrollment input");
    }
    auto transport = std::make_shared<NativeTransport>(loop, config, protocol::kMaximumNativeHandshakeBytes);
    asio::steady_timer deadline(loop.executor());
    deadline.expires_after(config.connectTimeout);
    deadline.async_wait([transport](const asio::error_code& error) {
        if (!error) {
            transport->cancel();
        }
    });
    try {
        co_await transport->connect();
        co_await transport->send(bytes);
        request.Clear();
        const auto response = protocol::decodeNativeAccepted(co_await transport->receive(), true);
        if (!response || !response->accepted()) {
            throw std::runtime_error("Native enrollment rejected");
        }
        deadline.cancel();
        transport->cancel();
        co_return EnrollmentIdentity{response->device_id(), response->credential()};
    } catch (...) {
        deadline.cancel();
        transport->cancel();
        throw;
    }
}
} // namespace ultraterimal::agent
