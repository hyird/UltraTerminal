#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <ruvia/core/EventLoopAttachment.h>
#include <ruvia/web/App.h>

#include "features/connection/native.transport.h"
#include "features/native/native.runtime.h"

namespace {
std::uint16_t nativePort(asio::io_context& io) {
    // Windows can reserve different ephemeral ranges for TCP and UDP.
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        asio::ip::tcp::acceptor tcp(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto port = tcp.local_endpoint().port();
        asio::ip::udp::socket udp(io, asio::ip::udp::v4());
        asio::error_code error;
        udp.bind({asio::ip::make_address("127.0.0.1"), port}, error);
        if (!error) {
            return port;
        }
    }
    throw std::runtime_error("Cannot reserve a shared TCP/UDP fixture port");
}
asio::awaitable<void> verify(ultraterimal::agent::NativeTransport& transport) {
    co_await transport.connect();
    if (transport.link() != ultraterimal::agent::NativeLink::kQuic) {
        throw std::runtime_error("Shutdown test did not establish QUIC");
    }
    // No business data is sent: this also covers a peer awaiting authentication.
    asio::steady_timer deadline(co_await asio::this_coro::executor);
    const auto expired = std::make_shared<bool>(false);
    deadline.expires_after(std::chrono::seconds(4));
    deadline.async_wait([&transport, expired](std::error_code error) {
        if (!error) {
            *expired = true;
            transport.cancel();
        }
    });
    ruvia::app().stop();
    bool closed = false;
    try {
        (void)co_await transport.receive();
    } catch (const std::exception&) {
        closed = true;
    }
    deadline.cancel();
    if (!closed || *expired) {
        throw std::runtime_error("Gateway shutdown did not notify its QUIC peer within four seconds");
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    try {
        asio::io_context ports;
        ultraterimal::server::ServerConfig config;
        config.nativeAddress = "127.0.0.1";
        config.nativePort = nativePort(ports);
        config.nativeCertificateFile = argv[1];
        config.nativeKeyFile = argv[2];
        ultraterimal::server::NativeServer server(config);
        asio::ip::tcp::acceptor webReservation(ports, {asio::ip::make_address("127.0.0.1"), 0});
        const auto webPort = webReservation.local_endpoint().port();
        webReservation.close();
        std::exception_ptr failure;
        std::jthread peer;
        auto& app = ruvia::app();
        app.listen({.address = "127.0.0.1", .http = webPort})
            .server({.workerCount = 1, .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kExternalOwner})
            .onStart([&] {
                server.start(app.workers().front());
                peer = std::jthread([&] {
                    asio::io_context io;
                    auto attachment = ruvia::attachEventLoop(io);
                    ultraterimal::agent::ConnectionConfig client;
                    client.host = "localhost";
                    client.port = std::to_string(config.nativePort);
                    client.caFile = argv[1];
                    ultraterimal::agent::NativeTransport transport(attachment.loop(), client);
                    asio::co_spawn(io, verify(transport), [&](std::exception_ptr error) {
                        failure = error;
                        transport.cancel();
                        app.stop();
                        attachment.stop();
                    });
                    attachment.run();
                });
            })
            .onStop([&] { server.stop(); })
            .run();
        peer.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Gateway lifecycle drained QUIC before closing UDP\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
