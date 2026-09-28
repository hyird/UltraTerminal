#include <array>
#include <asio/co_spawn.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <ruvia/core/EventLoopAttachment.h>

#include "features/connection/native.transport.h"
#include "ultraterimal/protocol/stream.protocol.h"

// Blocking reference peer runs on a test thread. The actual Agent transport
// runs asynchronously on its Ruvia loop.
namespace {
enum class Scenario { kNormal,
                      kServerClose,
                      kTransientLoss,
                      kSustainedLoss,
                      kStalledBody,
                      kStalledHeader,
                      kTrickleBody,
                      kIdleHeader };
Scenario scenario(std::string_view name) {
    if (name.empty()) {
        return Scenario::kNormal;
    }
    if (name == "server-close") {
        return Scenario::kServerClose;
    }
    if (name == "impaired") {
        return Scenario::kTransientLoss;
    }
    if (name == "sustained-loss") {
        return Scenario::kSustainedLoss;
    }
    if (name == "stalled-body") {
        return Scenario::kStalledBody;
    }
    if (name == "stalled-header") {
        return Scenario::kStalledHeader;
    }
    if (name == "idle-header") {
        return Scenario::kIdleHeader;
    }
    if (name == "trickle-body") {
        return Scenario::kTrickleBody;
    }
    throw std::invalid_argument("Unknown QUIC test scenario");
}
using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Connection = std::unique_ptr<SSL, decltype(&SSL_free)>;
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
int descriptor(asio::ip::udp::socket& socket) {
    const auto handle = socket.native_handle();
    require(handle <= static_cast<decltype(handle)>(std::numeric_limits<int>::max()), "Socket descriptor overflow");
    return static_cast<int>(handle);
}
std::string alpn() {
    std::string result(1, static_cast<char>(sizeof(ultraterimal::protocol::kNativeAlpn) - 1));
    return result + ultraterimal::protocol::kNativeAlpn;
}
void write(SSL* connection, const std::string& bytes) {
    const auto header = ultraterimal::protocol::encodeStreamHeader(bytes.size());
    require(header.has_value(), "Invalid outgoing QUIC record");
    std::string record(reinterpret_cast<const char*>(header->data()), header->size());
    record += bytes;
    std::size_t sent = 0;
    require(SSL_write_ex(connection, record.data(), record.size(), &sent) == 1 && sent == record.size(), "QUIC write failed");
}
void readExact(SSL* connection, char* data, std::size_t size) {
    while (size) {
        std::size_t received = 0;
        require(SSL_read_ex(connection, data, size, &received) == 1 && received > 0, "QUIC read failed");
        data += received;
        size -= received;
    }
}
std::string read(SSL* connection) {
    ultraterimal::protocol::StreamHeader header{};
    readExact(connection, reinterpret_cast<char*>(header.data()), header.size());
    const auto length = ultraterimal::protocol::decodeStreamHeader(header);
    require(length.has_value(), "Invalid incoming QUIC record");
    std::string bytes(*length, '\0');
    readExact(connection, bytes.data(), bytes.size());
    return bytes;
}
// A loopback-only datagram relay exercises real QUIC retransmission without
// changing host firewall or network settings. It never terminates encryption.
class ImpairedLink {
  public:
    explicit ImpairedLink(asio::ip::udp::endpoint server, bool sustained)
        : socket_(io_, {asio::ip::make_address("127.0.0.1"), 0}), server_(server), sustained_(sustained) {
        socket_.non_blocking(true);
        port_ = socket_.local_endpoint().port();
        thread_ = std::jthread([this](std::stop_token stop) {
            try {
                run(stop);
            } catch (...) {
                failure_ = std::current_exception();
            }
        });
    }
    unsigned short port() const { return port_; }
    void finish() {
        thread_.request_stop();
        thread_.join();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        require((sustained_ ? dropped_ > 3 : dropped_ == 3) && paused_, "QUIC impairment fixture did not inject every fault");
        std::cout << "Relay received=" << received_ << " deliberately_dropped=" << dropped_ << std::endl;
    }

  private:
    void run(std::stop_token stop) {
        using Clock = std::chrono::steady_clock;
        struct Packet {
            std::vector<char> data;
            asio::ip::udp::endpoint target;
            Clock::time_point due;
        };
        std::deque<Packet> pending;
        asio::ip::udp::endpoint client;
        Clock::time_point resume{};
        std::array<char, 65536> bytes{};
        while (!stop.stop_requested()) {
            for (unsigned batch = 0; batch < 64; ++batch) {
                asio::ip::udp::endpoint sender;
                std::error_code error;
                const auto size = socket_.receive_from(asio::buffer(bytes), sender, 0, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    break;
                }
                if (error) {
                    throw std::system_error(error);
                }
                if (sender != server_) {
                    client = sender;
                }
                ++received_;
                // Exercise sustained loss at the original ~3% rate without
                // synchronizing drops to a particular congestion window.
                const auto randomSample = static_cast<std::uint32_t>(received_ * 2654435761ULL ^ (received_ >> 7));
                if (received_ > 32 && ((sustained_ && randomSample % 31 == 0) || (!sustained_ && received_ <= 128 && received_ % 31 == 0))) {
                    ++dropped_;
                    continue;
                }
                if (!paused_ && received_ >= 64) {
                    paused_ = true;
                    resume = Clock::now() + std::chrono::seconds(1);
                }
                require(pending.size() < 2048, "QUIC impairment queue exceeded its bound");
                pending.push_back({std::vector<char>(bytes.data(), bytes.data() + size), sender == server_ ? client : server_, Clock::now() + std::chrono::milliseconds(2)});
            }
            const auto now = Clock::now();
            while (!pending.empty() && now >= resume && now >= pending.front().due) {
                socket_.send_to(asio::buffer(pending.front().data), pending.front().target);
                pending.pop_front();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    asio::io_context io_;
    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint server_;
    unsigned short port_ = 0;
    unsigned dropped_ = 0;
    unsigned received_ = 0;
    bool sustained_;
    bool paused_ = false;
    std::exception_ptr failure_;
    std::jthread thread_;
};
asio::awaitable<void> roundTrip(ultraterimal::agent::NativeTransport& transport, const std::atomic<bool>& uploaded, Scenario selected) {
    const bool serverClose = selected == Scenario::kServerClose;
    const bool sustained = selected == Scenario::kSustainedLoss;
    const bool impaired = sustained || selected == Scenario::kTransientLoss;
    co_await transport.connect();
    require(transport.link() == ultraterimal::agent::NativeLink::kQuic, "UDP was not selected");
    ultraterimal::wire::Envelope message;
    message.set_protocol_version(1);
    message.set_sequence(1);
    message.mutable_hello()->set_device_id("quic-fixture");
    message.mutable_hello()->set_agent_version("0.1.0");
    const auto bytes = ultraterimal::protocol::encodeMessage(message);
    require(bytes.has_value(), "Invalid QUIC fixture message");
    co_await transport.send(*bytes);
    require(co_await transport.receive() == *bytes, "QUIC round trip corrupted");
    if (selected == Scenario::kIdleHeader) {
        require(co_await transport.receive() == "late", "Idle connection did not receive its delayed record");
        co_await transport.close();
        std::cout << "PASS: idle connection survives eleven seconds without a record\n";
        co_return;
    }
    if (selected == Scenario::kStalledBody || selected == Scenario::kStalledHeader || selected == Scenario::kTrickleBody) {
        const auto started = std::chrono::steady_clock::now();
        bool rejected = false;
        try {
            (void)co_await transport.receive();
        } catch (const std::exception&) {
            rejected = true;
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const bool trickle = selected == Scenario::kTrickleBody;
        require(rejected && elapsed >= std::chrono::seconds(trickle ? 59 : 9) && elapsed < std::chrono::seconds(trickle ? 64 : 12), "Partial record did not enforce its expected deadline");
        require(transport.link() == ultraterimal::agent::NativeLink::kDisconnected, "Partial record retained its link");
        std::cout << (trickle ? "PASS: trickled record expires at sixty seconds despite byte progress\n" : "PASS: partial record without progress expires at ten seconds\n");
        co_return;
    }
    const std::string video(1024 * 1024, 'v');
    const auto uploadStarted = std::chrono::steady_clock::now();
    co_await transport.send(video);
    asio::steady_timer idle(co_await asio::this_coro::executor);
    if (sustained) {
        const auto deadline = uploadStarted + std::chrono::seconds(55);
        while (!uploaded.load() && std::chrono::steady_clock::now() < deadline) {
            idle.expires_after(std::chrono::milliseconds(50));
            co_await idle.async_wait(asio::use_awaitable);
        }
        std::cout << "Sustained-loss upload_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStarted).count() << std::endl;
    } else {
        idle.expires_after(std::chrono::milliseconds(impaired ? 2000 : 500));
        co_await idle.async_wait(asio::use_awaitable);
    }
    require(uploaded.load(), "QUIC upload stalled without an application reader");
    const auto downloadStarted = std::chrono::steady_clock::now();
    require(co_await transport.receive() == video, "Large QUIC record corrupted or stalled");
    if (sustained) {
        std::cout << "Sustained-loss remaining_download_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - downloadStarted).count() << std::endl;
    }
    co_await transport.send("ack");
    require(co_await transport.receive() == "done", "QUIC completion acknowledgement missing");
    require(transport.link() == ultraterimal::agent::NativeLink::kQuic, "Packet loss replaced the original QUIC connection");
    if (impaired) {
        // Continue exchanging fresh records after recovery on the same stream.
        // connect() is single-use and the fixture server accepts exactly once.
        for (unsigned sequence = 0; sequence < 8; ++sequence) {
            const auto recovered = std::string(65536, static_cast<char>('a' + sequence));
            co_await transport.send(recovered);
            require(co_await transport.receive() == recovered, "Recovered QUIC stream lost later data");
        }
        std::cout << "PASS: packet loss recovered on original connection, reconnects=0\n";
    }
    if (serverClose) {
        co_await transport.send("close");
        asio::steady_timer deadline(co_await asio::this_coro::executor);
        deadline.expires_after(std::chrono::seconds(2));
        const auto timedOut = std::make_shared<bool>(false);
        deadline.async_wait([&transport, timedOut](std::error_code error) {
            if (!error) {
                *timedOut = true;
                transport.cancel();
            }
        });
        bool closed = false;
        try {
            (void)co_await transport.receive();
        } catch (const std::exception&) {
            closed = true;
        }
        deadline.cancel();
        require(closed && !*timedOut, "Server QUIC close did not wake the pending Agent read within two seconds");
        co_return;
    }
    const auto closing = std::chrono::steady_clock::now();
    co_await transport.close();
    require(std::chrono::steady_clock::now() - closing < std::chrono::seconds(3), "QUIC orderly close exceeded its deadline");
    require(transport.link() == ultraterimal::agent::NativeLink::kDisconnected, "QUIC close retained an active link");
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        return 2;
    }
    try {
        const auto selectedScenario = scenario(argc == 4 ? argv[3] : "");
        const bool serverClose = selectedScenario == Scenario::kServerClose;
        const bool sustained = selectedScenario == Scenario::kSustainedLoss;
        const bool impaired = sustained || selectedScenario == Scenario::kTransientLoss;
        asio::io_context io;
        asio::ip::udp::socket serverSocket(io, {asio::ip::make_address("127.0.0.1"), 0});
        serverSocket.non_blocking(true);
        Context server(SSL_CTX_new(OSSL_QUIC_server_method()), SSL_CTX_free);
        require(server != nullptr, "QUIC server unavailable");
        require(SSL_CTX_use_certificate_chain_file(server.get(), argv[1]) == 1, "Server certificate failed");
        require(SSL_CTX_use_PrivateKey_file(server.get(), argv[2], SSL_FILETYPE_PEM) == 1, "Server key failed");
        auto protocols = alpn();
        SSL_CTX_set_alpn_select_cb(server.get(), [](SSL*, const unsigned char** out, unsigned char* length, const unsigned char* in, unsigned inLength, void* argument) -> int {
            const auto& offered = *static_cast<const std::string*>(argument);
            unsigned char* selected = nullptr;
            if (SSL_select_next_proto(&selected, length, reinterpret_cast<const unsigned char*>(offered.data()), static_cast<unsigned>(offered.size()), in, inLength) != OPENSSL_NPN_NEGOTIATED) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            *out = selected;
            return SSL_TLSEXT_ERR_OK; }, &protocols);
        Connection listener(SSL_new_listener(server.get(), 0), SSL_free);
        require(listener != nullptr && SSL_set_fd(listener.get(), descriptor(serverSocket)) == 1 && SSL_listen(listener.get()) == 1, "QUIC listen failed");
        std::exception_ptr serverFailure;
        std::atomic<bool> uploaded = false;
        std::jthread peer([&] {
            try {
                Connection connection(SSL_accept_connection(listener.get(), 0), SSL_free);
                require(connection != nullptr, "QUIC accept failed");
                const auto request = read(connection.get());
                const auto message = ultraterimal::protocol::decodeMessage(request);
                require(message && message->has_hello() && message->hello().device_id() == "quic-fixture", "QUIC protobuf request corrupted");
                write(connection.get(), request);
                if (selectedScenario == Scenario::kIdleHeader) {
                    std::this_thread::sleep_for(std::chrono::seconds(11));
                    write(connection.get(), "late");
                    char trailing = 0;
                    std::size_t received = 0;
                    require(SSL_read_ex(connection.get(), &trailing, 1, &received) == 0, "Idle fixture did not close normally");
                    return;
                }
                if (selectedScenario == Scenario::kStalledBody || selectedScenario == Scenario::kStalledHeader || selectedScenario == Scenario::kTrickleBody) {
                    const auto header = ultraterimal::protocol::encodeStreamHeader(1024);
                    require(header.has_value(), "Invalid stalled-body header");
                    std::string partial(reinterpret_cast<const char*>(header->data()), header->size());
                    partial += 'x';
                    if (selectedScenario == Scenario::kStalledHeader) {
                        partial.resize(1);
                    }
                    std::size_t sent = 0;
                    require(SSL_write_ex(connection.get(), partial.data(), partial.size(), &sent) == 1 && sent == partial.size(), "Cannot send partial record");
                    if (selectedScenario == Scenario::kTrickleBody) {
                        unsigned progressed = 0;
                        for (unsigned second = 0; second < 65; ++second) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            if (SSL_write_ex(connection.get(), "x", 1, &sent) != 1) {
                                break;
                            }
                            require(sent == 1, "Trickle peer did not send a byte");
                            ++progressed;
                        }
                        require(progressed >= 50, "Trickle peer did not sustain progress before total expiry");
                        std::cout << "Trickle progress samples=" << progressed << std::endl;
                    }
                    char trailing = 0;
                    std::size_t received = 0;
                    require(SSL_read_ex(connection.get(), &trailing, 1, &received) == 0, "Stalled client failed to close its connection");
                    return;
                }
                const auto video = read(connection.get());
                require(video == std::string(1024 * 1024, 'v'), "Large QUIC upload corrupted");
                uploaded.store(true);
                write(connection.get(), video);
                require(read(connection.get()) == "ack", "QUIC acknowledgement missing");
                write(connection.get(), "done");
                if (selectedScenario == Scenario::kTransientLoss || selectedScenario == Scenario::kSustainedLoss) {
                    for (unsigned sequence = 0; sequence < 8; ++sequence) {
                        const auto recovered = read(connection.get());
                        require(recovered == std::string(65536, static_cast<char>('a' + sequence)), "Recovered stream content changed");
                        write(connection.get(), recovered);
                    }
                }
                if (serverClose) {
                    require(read(connection.get()) == "close", "Server close request missing");
                    require(SSL_shutdown_ex(connection.get(), 0, nullptr, 0) == 1, "Server QUIC shutdown failed");
                    return;
                }
                char trailing = 0;
                std::size_t received = 0;
                bool observedOrderlyClose = false;
                const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < closeDeadline) {
                    ERR_clear_error();
                    const int closed = SSL_read_ex(connection.get(), &trailing, 1, &received);
                    const int reason = SSL_get_error(connection.get(), closed);
                    if (closed == 0) {
                        SSL_CONN_CLOSE_INFO closure{};
                        if (received == 0 && SSL_get_conn_close_info(connection.get(), &closure, sizeof(closure)) == 1) {
                            require(!(closure.flags & SSL_CONN_CLOSE_FLAG_LOCAL) && closure.error_code == 0,
                                    "Peer observed a non-orderly QUIC shutdown");
                            observedOrderlyClose = true;
                            break;
                        }
                    }
                    require(reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE, "Peer QUIC read failed during shutdown");
                    require(SSL_handle_events(connection.get()) == 1, "Peer QUIC event processing failed during shutdown");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!observedOrderlyClose) {
                    std::cout << "Peer did not observe QUIC close within five seconds; UDP close delivery is best-effort\n";
                }
            } catch (...) {
                serverFailure = std::current_exception();
            }
        });
        auto attachment = ruvia::attachEventLoop(io);
        std::unique_ptr<ImpairedLink> link;
        if (impaired) {
            link = std::make_unique<ImpairedLink>(serverSocket.local_endpoint(), sustained);
        }
        ultraterimal::agent::ConnectionConfig config;
        config.host = "localhost";
        config.port = std::to_string(link ? link->port() : serverSocket.local_endpoint().port());
        config.caFile = argv[1];
        ultraterimal::agent::NativeTransport transport(attachment.loop(), config);
        std::exception_ptr clientFailure;
        asio::co_spawn(io, roundTrip(transport, uploaded, selectedScenario), [&](std::exception_ptr failure) {
            clientFailure = failure;
            if (failure) {
                try {
                    std::rethrow_exception(failure);
                } catch (const std::exception& error) {
                    std::cerr << "QUIC client failure: " << error.what() << std::endl;
                }
            }
            transport.cancel();
            attachment.stop();
        });
        attachment.run();
        if (clientFailure) {
            std::rethrow_exception(clientFailure);
        }
        peer.join();
        if (serverFailure) {
            std::rethrow_exception(serverFailure);
        }
        if (link) {
            link->finish();
            std::cout << "PASS: QUIC record integrity after deterministic packet loss, forwarding delay and a 120ms pause\n";
        }
        std::cout << "PASS: static OpenSSL QUIC UDP, verified TLS, custom ALPN and protobuf round trip\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ERR_print_errors_fp(stderr);
        return 1;
    }
}
