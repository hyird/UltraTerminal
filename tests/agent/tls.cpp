#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include <openssl/err.h>
#include <openssl/sslerr.h>
#include <ruvia/core/EventLoopAttachment.h>

#include "features/connection/tls.transport.h"

asio::awaitable<void> exchange(ultraterimal::agent::TlsTransport& transport, const std::string& mode, asio::steady_timer& cancelTimer, std::chrono::steady_clock::time_point& cancelStarted) {
    co_await transport.connect();
    co_await transport.send(mode);
    if (mode == "cancel") {
        cancelStarted = std::chrono::steady_clock::now();
        cancelTimer.expires_after(std::chrono::milliseconds(100));
        cancelTimer.async_wait([&transport](const asio::error_code& error) {
            if (!error) {
                transport.cancel();
            }
        });
    }
    if (mode == "success" || mode == "slow_progress" || mode == "idle_header") {
        std::string payload(65536, '\0');
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(i % 251);
        }
        co_await transport.send(payload);
        if (co_await transport.receive() != payload || co_await transport.receive() != "tail") {
            throw std::runtime_error("Fragmented or coalesced record corrupted");
        }
        co_await transport.close();
    } else {
        co_await transport.receive();
        throw std::runtime_error("Unsafe record accepted");
    }
}
int main(int argc, char** argv) {
    if (argc != 5) {
        return 2;
    }
    const std::string mode = argv[4];
    const auto started = std::chrono::steady_clock::now();
    auto cancelStarted = std::chrono::steady_clock::time_point::max();
    try {
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        ultraterimal::agent::ConnectionConfig config;
        config.caFile = argv[1];
        config.host = argv[2];
        config.address = "127.0.0.1";
        config.port = argv[3];
        config.connectTimeout = std::chrono::seconds(mode == "timeout" ? 1 : 5);
        ultraterimal::agent::TlsTransport transport(attachment.loop(), config, 65536);
        asio::steady_timer cancelTimer(io);
        std::exception_ptr failure;
        asio::co_spawn(io, exchange(transport, mode, cancelTimer, cancelStarted), [&](std::exception_ptr error) {
            failure = error;
            attachment.stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (mode != "success" && mode != "slow_progress" && mode != "idle_header") {
            throw std::runtime_error("Expected rejection missing");
        }
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto* systemError = dynamic_cast<const asio::system_error*>(&error);
        const bool cancelled = systemError && systemError->code() == asio::error::operation_aborted;
        const bool certificate = systemError && ERR_GET_REASON(static_cast<unsigned long>(systemError->code().value())) == SSL_R_CERTIFICATE_VERIFY_FAILED;
        const bool cancelledPromptly = cancelStarted != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() - cancelStarted < std::chrono::seconds(1);
        const bool expected = ((mode == "hostname" || mode == "untrusted") && certificate) || (mode == "alpn" && reason == "Native TLS ALPN mismatch") || ((mode == "oversize" || mode == "zero") && reason == "Invalid native TLS record length") || (mode == "truncated" && systemError && (systemError->code() == asio::error::eof || systemError->code() == asio::ssl::error::stream_truncated)) || (mode == "cancel" && cancelled && cancelledPromptly) || (mode == "timeout" && cancelled && elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(3)) || ((mode == "stalled_body" || mode == "stalled_header") && cancelled && elapsed >= std::chrono::seconds(9) && elapsed < std::chrono::seconds(13)) || (mode == "trickle_body" && cancelled && elapsed >= std::chrono::seconds(59) && elapsed < std::chrono::seconds(64));
        if (!expected) {
            std::cerr << mode << ": " << reason << '\n';
            return 1;
        }
    }
    std::cout << mode << " passed\n";
    return 0;
}
