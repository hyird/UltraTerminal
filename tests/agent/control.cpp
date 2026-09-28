#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>

#include <ruvia/core/EventLoopAttachment.h>

#include "features/connection/control.runtime.h"
#include "platform/windows/credential.transport.h"

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        return 2;
    }
    try {
        const std::string mode = argv[4];
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        ultraterimal::agent::ConnectionConfig config;
        config.caFile = argv[1];
        config.host = "localhost";
        config.address = "127.0.0.1";
        config.port = argv[2];
        config.deviceId = "fixture-device";
        config.credentialFile = argv[3];
        config.connectTimeout = std::chrono::seconds(5);
        ultraterimal::agent::saveCredential(config.credentialFile, std::string(64, 'a'));
        ultraterimal::agent::ControlRuntime runtime(attachment.loop(), config, argc == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{});
        std::stop_source stopSource;
        std::stop_callback cancellation(stopSource.get_token(), [&] { runtime.stop(); });
        asio::steady_timer stop(io);
        const auto duration = mode == "stop_connect" || mode == "stop_backoff" ? std::chrono::milliseconds(150) : mode == "no_welcome" ? std::chrono::milliseconds(15000)
                                                                                                                                       : std::chrono::milliseconds(15000);
        stop.expires_after(duration);
        std::chrono::steady_clock::time_point stoppedAt;
        stop.async_wait([&](std::error_code error) {
            if (!error) {
                stoppedAt = std::chrono::steady_clock::now();
                // SCM invokes cancellation from a different thread, not the I/O loop.
                std::thread stopper([&] { stopSource.request_stop(); });
                stopper.join();
            }
        });
        std::exception_ptr failure;
        asio::co_spawn(io, runtime.run(), [&](std::exception_ptr error) { failure = error; stop.cancel(); attachment.stop(); });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (stoppedAt == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() - stoppedAt > std::chrono::milliseconds(500)) {
            throw std::runtime_error("Control runtime did not stop promptly");
        }
        const auto stats = runtime.statistics();
        if (stats.connected || !stats.attempts) {
            throw std::runtime_error("Incorrect stopped state");
        }
        if (mode == "reconnect" && (stats.connections < 2 || stats.acknowledgments < 3 || stats.rejectedSessions < 1)) {
            throw std::runtime_error("Reconnect, heartbeat, or session rejection was not exercised");
        }
        if (mode == "silent_heartbeat" && (stats.connections < 2 || stats.acknowledgments)) {
            throw std::runtime_error("Heartbeat watchdog did not force reconnect");
        }
        if (mode == "replay" && (stats.connections < 2 || stats.acknowledgments)) {
            throw std::runtime_error("Replayed control messages were not rejected");
        }
        if (mode == "no_welcome" && (stats.attempts < 2 || stats.connections)) {
            throw std::runtime_error("Welcome watchdog did not force reconnect");
        }
        if ((mode == "stop_connect" || mode == "stop_backoff") && stats.attempts != 1) {
            throw std::runtime_error("Unexpected retry before stop");
        }
        if (mode.starts_with("desktop_") && (stats.startedSessions != 1 || stats.completedSessions != 1 || stats.rejectedSessions)) {
            throw std::runtime_error("Control runtime did not start and join the desktop session");
        }
        if (mode == "desktop_disconnect" && stats.connections < 2) {
            throw std::runtime_error("Desktop teardown did not allow control reconnection");
        }
        std::cout << mode << ": attempts=" << stats.attempts << " connections=" << stats.connections
                  << " acknowledgments=" << stats.acknowledgments << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
