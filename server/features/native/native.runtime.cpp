#include "native.runtime.h"

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
#include <chrono>
#include <map>
#include <type_traits>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>
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
class NativeContext {
  public:
    NativeContext(ruvia::WebWorkerContext& context, DeviceIdentity device, std::shared_ptr<NativeChannel> channel)
        : context_(context), device_(std::move(device)), channel_(std::move(channel)) {}
    const ruvia::WorkerHandle& worker() const { return context_.worker(); }
    template <typename T>
    T& workerState() { return context_.workerState<T>(); }
    template <typename Fn>
    auto runBlocking(Fn&& operation) { return context_.runBlocking(std::forward<Fn>(operation)); }
    template <typename T>
    const T& requestState() const {
        if constexpr (std::is_same_v<T, DeviceIdentity>) {
            return device_;
        } else {
            throw std::logic_error("Native connections cannot impersonate browser users");
        }
    }
    std::string_view transportCloseCause() const { return nativeCloseCauseName(channel_->closeCause.load()); }

  private:
    ruvia::WebWorkerContext& context_;
    DeviceIdentity device_;
    std::shared_ptr<NativeChannel> channel_;
};
ruvia::Task<void> serveNative(ruvia::WebWorkerContext& context, std::shared_ptr<NativeChannel> channel) {
    try {
        channel->attach(context.worker());
        auto first = co_await channel->read();
        if (!first) {
            channel->abort();
            co_return;
        }
        auto request = protocol::decodeNativeConnect(first->payload());
        first.reset();
        if (!request) {
            channel->abort();
            co_return;
        }
        const auto purpose = request->purpose();
        auto& runtime = context.workerState<ServerRuntime>();
        wire::NativeAccepted response;
        response.set_protocol_version(protocol::kVersion);
        if (purpose == wire::NativeConnect::ENROLLMENT) {
            auto registration = co_await context.runBlocking([service = runtime.devices, code = request->enrollment_code(), name = request->device_name()] { return service.registerDevice(code, name); });
            request->Clear();
            if (registration) {
                response.set_accepted(true);
                response.set_device_id(registration->deviceId);
                response.set_credential(registration->credential);
            }
            co_await channel->binary(response.SerializeAsString());
        } else {
            auto device = co_await context.runBlocking([service = runtime.devices, credential = request->credential()] { return service.authenticate(credential); });
            request->Clear();
            if (device) {
                channel->authorizeNetwork(purpose == wire::NativeConnect::CONTROL ? protocol::kMaximumControlBytes : protocol::kMaximumMessageBytes);
                response.set_accepted(true);
                response.set_device_id(device->id);
            }
            co_await channel->binary(response.SerializeAsString());
            if (device) {
                NativeContext authenticated(context, std::move(*device), channel);
                if (purpose == wire::NativeConnect::CONTROL) {
                    co_await runControlConnection(authenticated, *channel);
                } else if (purpose == wire::NativeConnect::AGENT_UPDATE) {
                    co_await serveAgentUpdate(authenticated, *channel);
                } else {
                    co_await runDesktopChannel(authenticated, *channel, protocol::ChannelSource::kAgent);
                }
            }
        }
    } catch (const std::exception&) {
        // No peer payload, credential or database exception enters logs.
    }
    channel->abort();
}
struct NativePeer : std::enable_shared_from_this<NativePeer> {
    ruvia::EventLoop loop;
    std::unique_ptr<NativeStream> stream;
    asio::steady_timer authDeadline, readDeadline, writeDeadline;
    std::shared_ptr<NativeChannel> channel;
    std::shared_ptr<std::atomic<std::size_t>> budget;
    std::size_t maximum = protocol::kMaximumNativeHandshakeBytes;
    bool stopped = false;

    NativePeer(ruvia::EventLoop owner, std::unique_ptr<NativeStream> transport, std::shared_ptr<std::atomic<std::size_t>> receiveBudget)
        : loop(owner), stream(std::move(transport)), authDeadline(owner.executor()), readDeadline(owner.executor()), writeDeadline(owner.executor()), channel(std::make_shared<NativeChannel>(owner.handle())), budget(std::move(receiveBudget)) {}
    void bind() {
        channel->abortNetwork = [weak = weak_from_this(), owner = loop] {
            (void)owner.post([weak] {
                if (const auto self = weak.lock()) {
                    self->channel->recordCloseCause(NativeCloseCause::kServerAbort);
                    self->stop();
                }
            });
        };
        channel->authorizeNetwork = [weak = weak_from_this(), owner = loop](std::size_t limit) {
            const auto posted = owner.post([weak, limit] {
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
    asio::awaitable<void> read() {
        try {
            while (!stopped) {
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
                if (!channel->incoming->first.send(NativeMessage(std::move(payload))).accepted()) {
                    throw std::runtime_error("Native receive queue closed");
                }
                const auto credit = co_await ruvia::asAwaitable(channel->readCredit.second.receive());
                if (!credit.hasValue()) {
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
                auto result = co_await ruvia::asAwaitable(channel->outgoing.second.receive());
                if (!result.hasValue()) {
                    break;
                }
                auto bytes = std::move(result).takeValue();
                const auto header = protocol::encodeStreamHeader(bytes.size(), maximum);
                if (!header) {
                    throw std::runtime_error("Native write exceeds limit");
                }
                arm(writeDeadline, NativeCloseCause::kWriteTimeout);
                co_await stream->write(*header);
                co_await stream->write(std::as_bytes(std::span(bytes)));
                writeDeadline.cancel();
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
    static asio::awaitable<void> run(std::shared_ptr<NativePeer> self, ruvia::WebWorkerHandle businessWorker) {
        bool handshaken = false;
        try {
            self->bind();
            self->arm(self->authDeadline, NativeCloseCause::kAuthorizationTimeout);
            co_await self->stream->handshake();
            handshaken = true;
            auto posted = businessWorker.post([channel = self->channel](ruvia::WebWorkerContext& context) { return serveNative(context, channel); });
            if (!posted.accepted()) {
                throw std::runtime_error("Native business worker unavailable");
            }
            const auto initialized = co_await ruvia::asAwaitable(self->channel->initialized.second.receive());
            if (!initialized.hasValue() || self->stopped) {
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
    ruvia::EventLoopPool pool{{.loopCount = 1, .mailboxCapacity = 1024}};
    ruvia::EventLoop loop = pool.loop(0);
    asio::ssl::context tls{asio::ssl::context::tls_server};
    asio::ip::tcp::acceptor acceptor{loop.executor()};
    std::unique_ptr<QuicListener> quic;
    std::shared_ptr<std::atomic<std::size_t>> budget = std::make_shared<std::atomic<std::size_t>>(0);
    std::map<std::uint64_t, std::shared_ptr<NativePeer>> peers;
    ruvia::EventLoopStopRegistration stopping;
    ruvia::WebWorkerHandle business;
    std::string alpn;
    std::uint64_t sequence = 0;
    asio::steady_timer peersDone{loop.executor()};
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
            peer->stop();
        }
    }
    void adopt(std::unique_ptr<NativeStream> stream) {
        if (draining || peers.size() >= 128) {
            stream->cancel();
            return;
        }
        const auto id = ++sequence;
        auto peer = std::make_shared<NativePeer>(loop, std::move(stream), budget);
        peers.emplace(id, peer);
        asio::co_spawn(loop.executor(), NativePeer::run(peer, business), [self = shared_from_this(), id](std::exception_ptr) {
            self->peers.erase(id);
            self->peersDone.cancel();
        });
    }
    static asio::awaitable<void> drain(std::shared_ptr<State> self) {
        self->draining = true;
        asio::error_code ignored;
        self->acceptor.close(ignored);
        for (auto& [id, peer] : self->peers) {
            peer->stop();
        }
        // The UDP listener must keep pumping until peer I/O has joined and
        // each QUIC close has flushed its notification (bounded to two seconds).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!self->peers.empty() && std::chrono::steady_clock::now() < deadline) {
            self->peersDone.expires_at(deadline);
            co_await self->peersDone.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
        }
        self->stop();
    }
    static asio::awaitable<void> accept(std::shared_ptr<State> self) {
        while (self->acceptor.is_open()) {
            auto socket = co_await self->acceptor.async_accept(asio::use_awaitable);
            self->adopt(makeTlsStream(std::move(socket), self->tls));
        }
    }
};
NativeServer::NativeServer(const ServerConfig& config) : state_(std::make_shared<State>(config)) {
    state_->quic = std::make_unique<QuicListener>(state_->loop, config, [weak = std::weak_ptr<State>(state_)](std::unique_ptr<NativeStream> stream) {
        if (const auto state = weak.lock()) {
            state->adopt(std::move(stream));
        }
    });
}
NativeServer::~NativeServer() { stop(); }
void NativeServer::start(ruvia::WebWorkerHandle businessWorker) {
    state_->business = std::move(businessWorker);
    state_->stopping = state_->loop.onStop([weak = std::weak_ptr<State>(state_)] {
        if (const auto state = weak.lock()) {
            state->stop();
        }
    });
    state_->pool.start();
    state_->started = true;
    asio::co_spawn(state_->loop.executor(), State::accept(state_), [](std::exception_ptr) {});
    asio::co_spawn(state_->loop.executor(), state_->quic->run(), [](std::exception_ptr failure) {
        if (failure) {
            spdlog::warn("Native UDP listener stopped; TLS/TCP remains available");
        }
    });
}
void NativeServer::stop() {
    if (state_ && state_->started) {
        // Called by the application lifecycle thread, never the native loop.
        // Stop admission and drain transport tasks before stopping their executor.
        auto drained = asio::co_spawn(state_->loop.executor(), State::drain(state_), asio::use_future);
        try {
            drained.get();
        } catch (const std::exception&) {
            // The registered stop callback still closes every socket on failure.
        }
        state_->pool.stop();
        state_->pool.join();
        state_->started = false;
    }
}
} // namespace ultraterimal::server
