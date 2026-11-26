#include "native.runtime.h"

#include <algorithm>
#include <app.h>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <event_loop.h>
#include <map>
#include <one_shot.h>
#include <thread>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "config/server.runtime.h"
#include "features/control/control_exchange.runtime.h"
#include "features/desktop/desktop_exchange.runtime.h"
#include "native.transport.h"
#include "quic.transport.h"
#include "stream.transport.h"
#include "ultraterimal/protocol/stream.protocol.h"
#include "update.runtime.h"

namespace ultraterimal::server {
namespace {
// Bridge the public channel awaiter into the native Asio operation owner.
template <typename T>
ruvia::task<ruvia::wait_result<T>> receiveNative(ruvia::channel_receiver<T>& receiver) {
    co_return co_await receiver.receive();
}
template <typename T>
ruvia::task<ruvia::wait_result<T>> awaitOneShot(ruvia::one_shot_receiver<T>& receiver) {
    co_return co_await receiver.wait();
}
struct NativeAuthOutcome {
    bool accepted = false;
    std::string deviceId;
    std::string credential;
    std::optional<DeviceIdentity> device;
    bool aborted = false;
};
class NativeContext {
  public:
    NativeContext(ruvia::web_worker_context& context, DeviceIdentity device, std::shared_ptr<NativeChannel> channel)
        : context_(context), device_(std::move(device)), channel_(std::move(channel)) {}
    const ruvia::worker_ref& worker() const { return context_.worker(); }
    template <typename T>
    T& worker_state() { return context_.worker_state<T>(); }
    template <typename Fn>
    auto run_blocking(Fn&& operation) { return context_.run_blocking(std::forward<Fn>(operation)); }
    template <typename T>
    const T& request_state() const {
        if constexpr (std::is_same_v<T, DeviceIdentity>) {
            return device_;
        } else {
            throw std::logic_error("Native connections cannot impersonate browser users");
        }
    }
    std::string_view transport_close_cause() const { return nativeCloseCauseName(channel_->closeCause.load()); }

  private:
    ruvia::web_worker_context& context_;
    DeviceIdentity device_;
    std::shared_ptr<NativeChannel> channel_;
};
ruvia::task<void> serveAuthenticated(ruvia::web_worker_context& context, DeviceIdentity device, wire::NativeConnect::Purpose purpose, std::shared_ptr<NativeChannel> channel) {
    try {
        channel->attach(context.worker());
        NativeContext authenticated(context, std::move(device), channel);
        if (purpose == wire::NativeConnect::CONTROL) {
            co_await runControlConnection(authenticated, *channel);
        } else if (purpose == wire::NativeConnect::AGENT_UPDATE) {
            co_await serveAgentUpdate(authenticated, *channel);
        } else {
            co_await runDesktopChannel(authenticated, *channel, protocol::ChannelSource::kAgent);
        }
    } catch (const std::exception&) {
        // No peer payload, credential or database exception enters logs.
    }
    channel->abort();
}
struct NativePeer : std::enable_shared_from_this<NativePeer> {
    ruvia::event_loop loop;
    std::unique_ptr<NativeStream> stream;
    asio::steady_timer authDeadline, readDeadline, writeDeadline;
    std::shared_ptr<NativeChannel> channel;
    std::shared_ptr<std::atomic<std::size_t>> budget;
    std::size_t maximum = protocol::kMaximumNativeHandshakeBytes;
    bool stopped = false;

    NativePeer(ruvia::event_loop owner, std::unique_ptr<NativeStream> transport, std::shared_ptr<std::atomic<std::size_t>> receiveBudget)
        : loop(owner), stream(std::move(transport)), authDeadline(owner.executor()), readDeadline(owner.executor()), writeDeadline(owner.executor()), channel(std::make_shared<NativeChannel>(owner.worker())), budget(std::move(receiveBudget)) {}
    void bind() {
        channel->abortNetwork = [weak = weak_from_this(), owner = loop] {
            (void)owner.worker().post([weak] {
                if (const auto self = weak.lock()) {
                    self->channel->recordCloseCause(NativeCloseCause::kServerAbort);
                    self->stop();
                }
            });
        };
        channel->authorizeNetwork = [weak = weak_from_this(), owner = loop](std::size_t limit) {
            const auto posted = owner.worker().post([weak, limit] {
                if (const auto self = weak.lock()) {
                    self->maximum = limit;
                    self->authDeadline.cancel();
                }
            });
            if (!posted.accepted()) {
                throw std::runtime_error("Native authorization cancelled");
            }
        };
    }
    void stop() {
        if (stopped) {
            return;
        }
        stopped = true;
        authDeadline.cancel();
        readDeadline.cancel();
        writeDeadline.cancel();
        channel->closeQueues();
        stream->cancel();
    }
    void arm(asio::steady_timer& timer, NativeCloseCause cause) {
        timer.expires_after(std::chrono::seconds(10));
        timer.async_wait([self = shared_from_this(), cause](const asio::error_code& error) {
            if (!error) {
                self->channel->recordCloseCause(cause);
                self->stop();
            }
        });
    }
    asio::awaitable<std::shared_ptr<NativePayload>> readRecord() {
        protocol::StreamHeader header{};
        co_await stream->read(header);
        const auto size = protocol::decodeStreamHeader(header, maximum);
        if (!size) {
            throw std::runtime_error("Native record exceeds limit");
        }
        auto payload = std::make_shared<NativePayload>(*size, budget);
        arm(readDeadline, NativeCloseCause::kReadTimeout);
        co_await stream->read(std::as_writable_bytes(std::span(payload->bytes)));
        readDeadline.cancel();
        co_return payload;
    }
    asio::awaitable<void> writeRecord(std::string_view bytes) {
        const auto header = protocol::encodeStreamHeader(bytes.size(), maximum);
        if (!header) {
            throw std::runtime_error("Native write exceeds limit");
        }
        arm(writeDeadline, NativeCloseCause::kWriteTimeout);
        co_await stream->write(*header);
        co_await stream->write(std::as_bytes(std::span(bytes)));
        writeDeadline.cancel();
    }
    asio::awaitable<void> read() {
        try {
            while (!stopped) {
                auto payload = co_await readRecord();
                if (!channel->incoming->first.send(NativeMessage(std::move(payload))).accepted()) {
                    throw std::runtime_error("Native receive queue closed");
                }
                const auto credit = co_await ruvia::as_awaitable(receiveNative(channel->readCredit.second));
                if (!credit.has_value()) {
                    break;
                }
            }
        } catch (...) {
            if (!stopped) {
                channel->recordCloseCause(NativeCloseCause::kReadFailed);
            }
            stop();
            throw;
        }
        stop();
    }
    asio::awaitable<void> write() {
        try {
            while (!stopped) {
                auto result = co_await ruvia::as_awaitable(receiveNative(channel->outgoing.second));
                if (!result.has_value()) {
                    break;
                }
                auto bytes = std::move(result).take_value();
                co_await writeRecord(bytes);
                if (!channel->writeDone->first.send(true).accepted()) {
                    break;
                }
            }
        } catch (...) {
            if (!stopped) {
                channel->recordCloseCause(NativeCloseCause::kWriteFailed);
            }
            stop();
            throw;
        }
        stop();
    }
    static asio::awaitable<void> run(std::shared_ptr<NativePeer> self, std::uint64_t peerId) {
        bool handshaken = false;
        try {
            self->bind();
            self->arm(self->authDeadline, NativeCloseCause::kAuthorizationTimeout);
            co_await self->stream->handshake();
            handshaken = true;
            auto first = co_await self->readRecord();
            auto request = protocol::decodeNativeConnect(first->bytes);
            first.reset();
            const auto workers = ruvia::app().worker_refs();
            if (!request || workers.empty()) {
                throw std::runtime_error("Invalid native connect or missing workers");
            }
            const auto purpose = request->purpose();
            auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<NativeAuthOutcome>>(self->loop.worker());
            const auto authWorker = workers[peerId % workers.size()];
            const auto authPosted = authWorker.post([guard = WorkerRpcGuard<NativeAuthOutcome>(std::move(completion)), req = std::move(*request)](ruvia::web_worker_context& context) mutable -> ruvia::task<void> {
                WorkerRpcOutcome<NativeAuthOutcome> rpc;
                NativeAuthOutcome outcome;
                try {
                    auto& runtime = context.worker_state<ServerRuntime>();
                    if (req.purpose() == wire::NativeConnect::ENROLLMENT) {
                        auto reg = ((co_await context.run_blocking([service = runtime.devices, code = req.enrollment_code(), name = req.device_name()] { return service.registerDevice(code, name); })).take_value());
                        req.Clear();
                        if (reg) {
                            outcome.accepted = true;
                            outcome.deviceId = std::move(reg->deviceId);
                            outcome.credential = std::move(reg->credential);
                        }
                    } else {
                        auto dev = ((co_await context.run_blocking([service = runtime.devices, credential = req.credential()] { return service.authenticate(credential); })).take_value());
                        req.Clear();
                        if (dev) {
                            outcome.accepted = true;
                            outcome.deviceId = dev->id;
                            outcome.device = std::move(dev);
                        }
                    }
                    rpc.value.emplace(std::move(outcome));
                } catch (...) {
                    rpc.aborted = true;
                }
                guard.answer(std::move(rpc));
                co_return;
            });
            if (!authPosted.accepted()) {
                throw std::runtime_error("Native authentication worker unavailable");
            }
            const auto authResult = co_await ruvia::as_awaitable(awaitOneShot(receiver));
            if (!authResult.has_value() || authResult.value().aborted || !authResult.value().value || self->stopped) {
                self->stop();
                co_await self->stream->close();
                co_return;
            }
            const auto& outcome = *authResult.value().value;
            wire::NativeAccepted response;
            response.set_protocol_version(protocol::kVersion);
            if (outcome.accepted) {
                response.set_accepted(true);
                response.set_device_id(outcome.deviceId);
                if (!outcome.credential.empty()) {
                    response.set_credential(outcome.credential);
                }
            }
            co_await self->writeRecord(response.SerializeAsString());
            if (!outcome.accepted || purpose == wire::NativeConnect::ENROLLMENT || !outcome.device) {
                self->stop();
                co_await self->stream->close();
                co_return;
            }
            self->maximum = purpose == wire::NativeConnect::CONTROL ? protocol::kMaximumControlBytes : protocol::kMaximumMessageBytes;
            self->authDeadline.cancel();
            auto device = *outcome.device;
            const auto targetWorker = purpose == wire::NativeConnect::CONTROL ? ruvia::app().worker_for(device.id) : workers[peerId % workers.size()];
            auto posted = targetWorker.post([channel = self->channel, device = std::move(device), purpose](ruvia::web_worker_context& context) {
                return serveAuthenticated(context, std::move(device), purpose, std::move(channel));
            });
            if (!posted.accepted()) {
                throw std::runtime_error("Native business worker unavailable");
            }
            const auto initialized = co_await ruvia::as_awaitable(receiveNative(self->channel->initialized.second));
            if (!initialized.has_value() || self->stopped) {
                self->stop();
                co_await self->stream->close();
                co_return;
            }
            using namespace asio::experimental::awaitable_operators;
            co_await (self->read() && self->write());
        } catch (const std::exception&) {
            if (!self->stopped) {
                self->channel->recordCloseCause(handshaken ? NativeCloseCause::kRuntimeFailed : NativeCloseCause::kHandshakeFailed);
            }
        }
        self->stop();
        co_await self->stream->close();
    }
};
} // namespace
struct NativeServer::State : std::enable_shared_from_this<State> {
    ruvia::blocking_pool blockingPool{{.thread_count_ = std::max<std::size_t>(1u, std::thread::hardware_concurrency()), .queue_capacity_ = std::max<std::size_t>(64u, std::thread::hardware_concurrency() * 16u)}};
    ruvia::event_loop_pool pool{{.loop_count_ = 0, .queue_capacity_ = 1024, .blocking_pool_ = &blockingPool}};
    ruvia::event_loop acceptLoop = pool.loop(0);
    asio::ssl::context tls{asio::ssl::context::tls_server};
    asio::ip::tcp::acceptor acceptor{acceptLoop.executor()};
    std::unique_ptr<QuicListener> quic;
    std::shared_ptr<std::atomic<std::size_t>> budget = std::make_shared<std::atomic<std::size_t>>(0);
    std::map<std::uint64_t, std::shared_ptr<NativePeer>> peers;
    ruvia::event_loop_stop_registration stopping;
    std::string alpn;
    std::uint64_t sequence = 0;
    asio::steady_timer peersDone{acceptLoop.executor()};
    bool started = false, draining = false;
    explicit State(const ServerConfig& config) {
        if (SSL_CTX_set_min_proto_version(tls.native_handle(), TLS1_3_VERSION) != 1) {
            throw std::runtime_error("Native TLS 1.3 unavailable");
        }
        tls.use_certificate_chain_file(config.nativeCertificateFile.string());
        tls.use_private_key_file(config.nativeKeyFile.string(), asio::ssl::context::pem);
        alpn.assign(1, static_cast<char>(sizeof(protocol::kNativeAlpn) - 1));
        alpn += protocol::kNativeAlpn;
        SSL_CTX_set_alpn_select_cb(tls.native_handle(), [](SSL*, const unsigned char** output, unsigned char* size, const unsigned char* input, unsigned inputSize, void* argument) -> int {
            const auto& protocols = static_cast<State*>(argument)->alpn;
            unsigned char* selected = nullptr;
            if (SSL_select_next_proto(&selected, size, reinterpret_cast<const unsigned char*>(protocols.data()), static_cast<unsigned>(protocols.size()), input, inputSize) != OPENSSL_NPN_NEGOTIATED) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            *output = selected;
            return SSL_TLSEXT_ERR_OK; }, this);
        const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(config.nativeAddress), config.nativePort);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(64);
    }
    void stop() {
        asio::error_code ignored;
        acceptor.close(ignored);
        if (quic) {
            quic->stop();
        }
        for (auto& [id, peer] : peers) {
            if (peer->loop.worker().is_current()) {
                peer->stop();
            } else {
                (void)peer->loop.defer_cleanup([peer] { peer->stop(); });
            }
        }
    }
    void adopt(std::unique_ptr<NativeStream> stream, ruvia::event_loop targetLoop) {
        if (draining || peers.size() >= 128) {
            stream->cancel();
            return;
        }
        const auto id = ++sequence;
        auto peer = std::make_shared<NativePeer>(targetLoop, std::move(stream), budget);
        peers.emplace(id, peer);
        asio::co_spawn(targetLoop.executor(), NativePeer::run(peer, id), [self = shared_from_this(), owner = acceptLoop, id](std::exception_ptr) {
            if (owner.worker().is_current()) {
                self->peers.erase(id);
                self->peersDone.cancel();
            } else {
                (void)owner.defer_cleanup([self, id] {
                    self->peers.erase(id);
                    self->peersDone.cancel();
                });
            }
        });
    }
    static asio::awaitable<void> drain(std::shared_ptr<State> self) {
        self->draining = true;
        asio::error_code ignored;
        self->acceptor.close(ignored);
        for (auto& [id, peer] : self->peers) {
            if (peer->loop.worker().is_current()) {
                peer->stop();
            } else {
                (void)peer->loop.defer_cleanup([peer] { peer->stop(); });
            }
        }
        // The UDP listener must keep pumping until peer I/O has joined and
        // each QUIC close has flushed its notification (bounded to three seconds).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!self->peers.empty() && std::chrono::steady_clock::now() < deadline) {
            self->peersDone.expires_at(deadline);
            co_await self->peersDone.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        self->stop();
    }
    static asio::awaitable<void> accept(std::shared_ptr<State> self) {
        while (self->acceptor.is_open()) {
            auto targetLoop = self->pool.next_loop();
            asio::ip::tcp::socket socket(targetLoop.executor());
            co_await self->acceptor.async_accept(socket, asio::use_awaitable);
            self->adopt(makeTlsStream(std::move(socket), self->tls), std::move(targetLoop));
        }
    }
};
NativeServer::NativeServer(const ServerConfig& config) : state_(std::make_shared<State>(config)) {
    state_->quic = std::make_unique<QuicListener>(state_->acceptLoop, config, [weak = std::weak_ptr<State>(state_)](std::unique_ptr<NativeStream> stream) {
        if (const auto state = weak.lock()) {
            state->adopt(std::move(stream), state->acceptLoop);
        }
    });
}
NativeServer::~NativeServer() { stop(); }
void NativeServer::start() {
    state_->stopping = state_->acceptLoop.on_stop([weak = std::weak_ptr<State>(state_)]() -> ruvia::task<void> {
        if (const auto state = weak.lock()) {
            state->stop();
        }
        co_return;
    });
    state_->pool.start();
    state_->started = true;
    asio::co_spawn(state_->acceptLoop.executor(), State::accept(state_), [](std::exception_ptr) {});
    asio::co_spawn(state_->acceptLoop.executor(), state_->quic->run(), [](std::exception_ptr failure) {
        if (failure) {
            spdlog::warn("Native UDP listener stopped; TLS/TCP remains available");
        }
    });
}
void NativeServer::stop() {
    if (state_ && state_->started) {
        // Called by the application lifecycle thread, never the native loop.
        // Stop admission and drain transport tasks before stopping their executor.
        auto drained = asio::co_spawn(state_->acceptLoop.executor(), State::drain(state_), asio::use_future);
        try {
            drained.get();
        } catch (const std::exception&) {
            // The registered stop callback still closes every socket on failure.
        }
        state_->pool.request_stop();
        state_->pool.join();
        state_->blockingPool.join();
        state_->started = false;
    }
}
} // namespace ultraterimal::server
