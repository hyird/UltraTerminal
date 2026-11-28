#pragma once

#include <chrono>
#include <exception>
#include <one_shot.h>
#include <optional>
#include <task.h>
#include <task_scope.h>
#include <tuple>
#include <utility>

#include "config/server.runtime.h"
#include "modules/identity/identity.service.h"

namespace ultraterimal::server {
namespace {
using Clock = std::chrono::steady_clock;
struct ChannelState {
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    protocol::ChannelSource source = protocol::ChannelSource::kBrowser;
    std::optional<ChannelAuthorization> authorization;
    ruvia::web_worker_ref sessionWorker;
    std::shared_ptr<DesktopSession> session;
    std::shared_ptr<DesktopEndpoint> endpoint;
    bool owner = false;
    DesktopCloseOrigin origin() const {
        if (!authorization) {
            return DesktopCloseOrigin::kServer;
        }
        const bool video = authorization->purpose == wire::DESKTOP_VIDEO;
        if (source == protocol::ChannelSource::kAgent) {
            return video ? DesktopCloseOrigin::kAgentVideo : DesktopCloseOrigin::kAgentInput;
        }
        return video ? DesktopCloseOrigin::kBrowserVideo : DesktopCloseOrigin::kBrowserInput;
    }
};
inline void closeChannelSession(ChannelState& state, DesktopCloseReason reason) {
    if (!state.session) {
        return;
    }
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        state.session->close(reason, state.origin());
        return;
    }
    (void)state.sessionWorker.post([session = state.session, reason, origin = state.origin()](ruvia::web_worker_context&) -> ruvia::task<void> {
        session->close(reason, origin);
        co_return;
    });
}
inline void notifyWriteCompleted(ChannelState& state, const DesktopDelivery& delivery, std::chrono::steady_clock::duration elapsed) {
    if (delivery.ready()) {
        return;
    }
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        state.session->writeCompleted(delivery, elapsed);
        return;
    }
    const auto timing = DesktopSession::captureWriteTiming(delivery, elapsed);
    if (!timing.browserVideo) {
        return;
    }
    (void)state.sessionWorker.post([session = state.session, timing](ruvia::web_worker_context&) -> ruvia::task<void> {
        session->writeCompleted(timing);
        co_return;
    });
}
inline void logSessionHealth(ChannelState& state) {
    if (!state.session) {
        return;
    }
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        state.session->logHealth();
        return;
    }
    (void)state.sessionWorker.post([session = state.session](ruvia::web_worker_context&) -> ruvia::task<void> {
        session->logHealth();
        co_return;
    });
}
inline ruvia::task<bool> forwardToSession(const ruvia::worker_ref& caller, ChannelState& state, wire::Envelope message, ruvia::stop_token stop) {
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        co_return co_await state.session->forward(state.endpoint, message, std::move(stop));
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<bool>>(caller);
    const auto posted = state.sessionWorker.post([guard = WorkerRpcGuard<bool>(std::move(completion)), session = state.session, endpoint = state.endpoint, message = std::move(message), stop](ruvia::web_worker_context&) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<bool> out;
        try {
            out.value.emplace(co_await session->forward(std::move(endpoint), message, std::move(stop)));
        } catch (...) {
            out.failure = std::current_exception();
        }
        guard.answer(std::move(out));
    });
    if (!posted.accepted()) {
        co_return false;
    }
    auto waited = co_await receiver.wait({.stop_token_ = std::move(stop)});
    if (!waited.has_value()) {
        co_return false;
    }
    auto outcome = std::move(waited).take_value();
    if (outcome.failure) {
        std::rethrow_exception(outcome.failure);
    }
    co_return !outcome.aborted && outcome.value && *outcome.value;
}
template <typename Connection>
ruvia::task<void> writeChannel(Connection& socket, ChannelState& state, ruvia::task_scope& scope) {
    try {
        while (!scope.stop_requested()) {
            auto delivery = co_await state.endpoint->receive(scope.token());
            if (scope.stop_requested()) {
                break;
            }
            if (!delivery || state.session->closed()) {
                if (!state.session->closed()) {
                    closeChannelSession(state, DesktopCloseReason::kQueueClosed);
                }
                socket.abort();
                break;
            }
            if (delivery->ready()) {
                wire::Envelope ready;
                ready.set_protocol_version(protocol::kVersion);
                ready.set_sequence(1);
                ready.set_session_id(state.authorization->sessionId);
                ready.set_channel_id(state.authorization->channelId);
                ready.mutable_channel_ready()->set_purpose(state.authorization->purpose);
                ready.mutable_channel_ready()->set_desktop_optimization_contract(2);
                ready.mutable_channel_ready()->set_continuous_frames(true);
                if (state.authorization->purpose == wire::DESKTOP_INPUT) {
                    ready.mutable_channel_ready()->set_desktop_audio(true);
                }
                auto bytes = protocol::encodeMessage(ready);
                if (!bytes) {
                    throw std::runtime_error("Invalid channel readiness");
                }
                co_await socket.binary(*bytes);
            } else {
                const auto writeStarted = Clock::now();
                co_await socket.binary(delivery->bytes());
                notifyWriteCompleted(state, *delivery, Clock::now() - writeStarted);
            }
        }
    } catch (const std::exception&) {
        closeChannelSession(state, DesktopCloseReason::kWriteFailed);
        socket.abort();
    }
}
template <typename Context, typename Connection>
ruvia::task<void> watchChannel(Context& context, Connection& socket, ChannelState& state, ruvia::task_scope& scope) {
    auto nextAuthorization = Clock::now();
    auto nextHealth = Clock::now() + std::chrono::seconds(30);
    try {
        while (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1), scope.token()) == ruvia::sleep_status::elapsed) {
            if ((!state.session || !state.session->ready()) && Clock::now() >= state.deadline) {
                if (state.session) {
                    closeChannelSession(state, DesktopCloseReason::kSetupTimeout);
                }
                socket.abort();
                co_return;
            }
            if (state.session && state.session->closed()) {
                socket.abort();
                co_return;
            }
            if (state.owner && state.session && Clock::now() >= nextHealth) {
                logSessionHealth(state);
                nextHealth = Clock::now() + std::chrono::seconds(30);
            }
            if (state.owner && Clock::now() >= nextAuthorization) {
                auto& localRuntime = context.template worker_state<ServerRuntime>();
                const bool authorized = co_await localRuntime.sessions.authorized(context.db(), state.authorization->sessionId);
                if (!authorized) {
                    (void)co_await localRuntime.sessions.close(context.db(), state.authorization->userId, state.authorization->sessionId, SessionEnd::kPermissionRevoked);
                    closeChannelSession(state, DesktopCloseReason::kPermissionRevoked);
                    socket.abort();
                    co_return;
                }
                nextAuthorization = Clock::now() + std::chrono::seconds(5);
            }
        }
    } catch (const std::exception&) {
        if (state.session) {
            closeChannelSession(state, DesktopCloseReason::kAuthorizationCheckFailed);
        }
        socket.abort();
    }
}
template <typename Context, typename Connection>
ruvia::task<void> exchangeChannel(Context& context, Connection& socket, protocol::ChannelSource source, ChannelState& state, ruvia::task_scope& scope, std::optional<wire::ChannelPurpose> expectedPurpose) {
    auto incoming = co_await socket.read();
    if (!incoming || !incoming->binary() || incoming->payload().size() > protocol::kMaximumControlBytes) {
        co_return;
    }
    auto open = protocol::decodeMessage(incoming->payload());
    incoming.reset();
    if (!open || !open->has_channel_open() || open->sequence() != 1 || open->generation() || open->frame_id() || open->base_frame_id() ||
        !protocol::purposeMatches(wire::DESKTOP, open->channel_open().purpose()) || (expectedPurpose && open->channel_open().purpose() != *expectedPurpose)) {
        co_return;
    }
    auto& runtime = context.template worker_state<ServerRuntime>();
    if (source == protocol::ChannelSource::kBrowser) {
        state.authorization = co_await runtime.sessions.consumeBrowser(context.db(), context.template request_state<UserIdentity>(), open->session_id(), open->channel_id(), open->channel_open().purpose(), open->channel_open().ticket());
    } else {
        state.authorization = co_await runtime.sessions.consumeAgent(context.db(), context.template request_state<DeviceIdentity>(), open->session_id(), open->channel_id(), open->channel_open().purpose(), open->channel_open().ticket());
    }
    open->Clear(); // The live runtime does not retain bearer tickets.
    if (!state.authorization || scope.stop_requested()) {
        co_return;
    }
    state.sessionWorker = resolveWebWorker(state.authorization->sessionId);
    const auto attachLocal = [auth = *state.authorization, source, ioWorker = context.worker()](ServerRuntime& targetRuntime, const ruvia::worker_ref& ownerWorker) {
        auto found = targetRuntime.desktopSessions.find(auth.sessionId);
        std::shared_ptr<DesktopSession> session;
        bool owner = false;
        if (found == targetRuntime.desktopSessions.end()) {
            session = std::make_shared<DesktopSession>(ownerWorker, targetRuntime.shared->desktopBudget);
            targetRuntime.desktopSessions.emplace(auth.sessionId, session);
            owner = true;
        } else {
            session = found->second;
        }
        auto endpoint = session->attach(auth, source, ioWorker);
        return std::tuple{std::move(session), std::move(endpoint), owner};
    };
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        auto [session, endpoint, owner] = attachLocal(runtime, context.worker());
        state.session = std::move(session);
        state.endpoint = std::move(endpoint);
        state.owner = owner;
    } else {
        using Attached = std::tuple<std::shared_ptr<DesktopSession>, std::shared_ptr<DesktopEndpoint>, bool>;
        auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<Attached>>(context.worker());
        const auto posted = state.sessionWorker.post([guard = WorkerRpcGuard<Attached>(std::move(completion)), attachLocal](ruvia::web_worker_context& ctx) mutable -> ruvia::task<void> {
            WorkerRpcOutcome<Attached> out;
            try {
                out.value.emplace(attachLocal(ctx.worker_state<ServerRuntime>(), ctx.worker()));
            } catch (...) {
                out.failure = std::current_exception();
            }
            guard.answer(std::move(out));
            co_return;
        });
        if (!posted.accepted()) {
            co_return;
        }
        auto waited = co_await receiver.wait({.stop_token_ = scope.token()});
        if (!waited.has_value()) {
            co_return;
        }
        auto outcome = std::move(waited).take_value();
        if (outcome.failure) {
            std::rethrow_exception(outcome.failure);
        }
        if (outcome.aborted || !outcome.value) {
            co_return;
        }
        auto [session, endpoint, owner] = std::move(*outcome.value);
        state.session = std::move(session);
        state.endpoint = std::move(endpoint);
        state.owner = owner;
    }
    if (!state.endpoint) {
        closeChannelSession(state, DesktopCloseReason::kAttachRejected);
        co_return;
    }
    state.deadline = Clock::now() + std::chrono::seconds(60);
    scope.spawn(writeChannel(socket, state, scope));
    while (true) {
        incoming = co_await socket.read();
        if (!incoming) {
            closeChannelSession(state, DesktopCloseReason::kPeerClosed);
            break;
        }
        if (!incoming->binary()) {
            closeChannelSession(state, DesktopCloseReason::kPeerNonBinary);
            break;
        }
        auto message = protocol::decodeMessage(incoming->payload());
        if (!message) {
            closeChannelSession(state, DesktopCloseReason::kMalformedMessage);
            break;
        }
        if (!(co_await forwardToSession(context.worker(), state, std::move(*message), scope.token()))) {
            break;
        }
    }
}
template <typename Context, typename Connection>
ruvia::task<void> channel(Context& context, Connection& socket, protocol::ChannelSource source, std::optional<wire::ChannelPurpose> expectedPurpose = std::nullopt) {
    ChannelState state;
    state.source = source;
    auto worker = context.worker();
    {
        const auto scopeWorker = worker;
        ruvia::task_scope scope(scopeWorker);
        bool scopeJoined = false;
        std::exception_ptr scopeFailure;
        try {
            scope.spawn(watchChannel(context, socket, state, scope));
            try {
                co_await exchangeChannel(context, socket, source, state, scope, expectedPurpose);
            } catch (const std::exception&) {
                if (state.session) {
                    closeChannelSession(state, DesktopCloseReason::kReadFailed);
                }
            }
            if (state.session) {
                closeChannelSession(state, DesktopCloseReason::kPeerClosed);
            }
            socket.abort();
            scope.request_stop();
            scopeJoined = true;
            co_await scope.join();
            if (state.authorization) {
                auto& runtime = context.template worker_state<ServerRuntime>();
                try {
                    (void)co_await runtime.sessions.close(context.db(), state.authorization->userId, state.authorization->sessionId, SessionEnd::kFailure);
                } catch (const std::exception&) {
                    // A later permission check, reconnect or gateway recovery closes the persisted session.
                }
                if (state.owner) {
                    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
                        runtime.desktopSessions.erase(state.authorization->sessionId);
                    } else {
                        (void)state.sessionWorker.post([sessionId = state.authorization->sessionId](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
                            ctx.worker_state<ServerRuntime>().desktopSessions.erase(sessionId);
                            co_return;
                        });
                    }
                    sendDeviceSessionClose(runtime, state.authorization->deviceId, state.authorization->sessionId, "channel_closed");
                }
            }

        } catch (...) {
            scopeFailure = std::current_exception();
            if (state.session) {
                closeChannelSession(state, DesktopCloseReason::kReadFailed);
            }
            socket.abort();
        }
        if (!scopeJoined) {
            scope.request_stop();
            try {
                co_await scope.join();
            } catch (...) {
                if (!scopeFailure) {
                    scopeFailure = std::current_exception();
                }
            }
        }
        if (scopeFailure) {
            std::rethrow_exception(scopeFailure);
        }
    }
}
} // namespace
template <typename Context, typename Connection>
ruvia::task<void> runDesktopChannel(Context& context, Connection& socket, protocol::ChannelSource source, std::optional<wire::ChannelPurpose> expectedPurpose = std::nullopt) {
    co_await channel(context, socket, source, expectedPurpose);
}
} // namespace ultraterimal::server
