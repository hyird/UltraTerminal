#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <ruvia/core/EventLoopAttachment.h>

#include "features/connection/native.transport.h"
#include "ultraterimal/protocol/native.protocol.h"

asio::awaitable<void> exchange(ultraterimal::agent::NativeTransport& transport, bool gateway, bool tcp) {
    co_await transport.connect();
    if (gateway) {
        if (transport.link() != (tcp ? ultraterimal::agent::NativeLink::kTls : ultraterimal::agent::NativeLink::kQuic)) {
            throw std::runtime_error("Production gateway selected the wrong transport");
        }
        ultraterimal::wire::NativeConnect request;
        request.set_protocol_version(1);
        request.set_purpose(ultraterimal::wire::NativeConnect::CONTROL);
        request.set_credential(std::string(64, 'a'));
        co_await transport.send(request.SerializeAsString());
        const auto response = ultraterimal::protocol::decodeNativeAccepted(co_await transport.receive(), false);
        if (!response || response->accepted()) {
            throw std::runtime_error("Gateway accepted an unknown native credential");
        }
        co_return;
    }
    if (transport.link() != ultraterimal::agent::NativeLink::kTls) {
        throw std::runtime_error("Unavailable UDP did not select TLS");
    }
    const std::string bytes("native\0binary", 13);
    co_await transport.send(bytes);
    if (co_await transport.receive() != bytes) {
        throw std::runtime_error("Fallback binary round trip corrupted");
    }
}
int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        return 2;
    }
    try {
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        ultraterimal::agent::ConnectionConfig config;
        config.host = argc == 6 ? argv[4] : "localhost";
        config.address = argc == 6 ? argv[5] : "127.0.0.1";
        config.port = argv[2];
        config.caFile = argv[1];
        ultraterimal::agent::NativeTransport transport(attachment.loop(), config);
        const bool cancel = std::string_view(argv[3]) == "cancel";
        const bool tcp = std::string_view(argv[3]) == "gateway-tcp";
        const bool gateway = std::string_view(argv[3]) == "gateway" || tcp;
        const bool untrusted = std::string_view(argv[3]) == "untrusted";
        asio::steady_timer timer(io);
        if (cancel) {
            timer.expires_after(std::chrono::milliseconds(100));
            timer.async_wait([&](const asio::error_code& error) {
                if (!error) {
                    transport.cancel();
                }
            });
        }
        std::exception_ptr failure;
        asio::co_spawn(io, exchange(transport, gateway || untrusted, tcp), [&](std::exception_ptr error) {
            failure = error;
            transport.cancel();
            attachment.stop();
        });
        attachment.run();
        if (cancel) {
            if (!failure) {
                throw std::runtime_error("Cancellation did not end connect");
            }
            try {
                std::rethrow_exception(failure);
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) != "Native connection cancelled") {
                    throw;
                }
            }
        } else if (untrusted) {
            if (!failure) {
                throw std::runtime_error("Untrusted QUIC identity was accepted");
            }
            try {
                std::rethrow_exception(failure);
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) != "QUIC certificate verification failed") {
                    throw;
                }
            }
        } else if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << (cancel ? "PASS: cancellation prevents TCP fallback\n" : untrusted ? "PASS: untrusted QUIC identity fails without TCP downgrade\n" : tcp ? "PASS: production gateway TCP fallback and credential rejection\n" : gateway ? "PASS: production gateway QUIC and credential rejection\n" : "PASS: unavailable UDP falls back to verified TLS/TCP\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
