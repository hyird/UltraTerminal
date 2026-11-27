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
struct TerminalChannelState {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    protocol::ChannelSource source = protocol::ChannelSource::kBrowser;
    std::optional<ChannelAuthorization> authorization;
    ruvia::web_worker_ref sessionWorker;
    std::shared_ptr<TerminalSession> session;
    std::shared_ptr<TerminalEndpoint> endpoint;
    bool owner = false;
};

inline void closeTerminalChannelSession(TerminalChannelState& state, TerminalCloseReason reason) {
    if (!state.session) {
        return;
    }
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        state.session->close(reason);
        return;
    }
    (void)state.sessionWorker.post([session = state.session, reason](ruvia::web_worker_context&) -> ruvia::task<void> {
        session->close(reason);
        co_return;
    });
}

inline ruvia::task<bool> forwardToTerminalSession(const ruvia::worker_ref& caller, TerminalChannelState& state, wire::Envelope message, ruvia::stop_token stop) {
    if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
        co_return co_await state.session->forward(state.endpoint, std::move(message), std::move(stop));
    }
    auto [completion, receiver] = ruvia::make_one_shot<WorkerRpcOutcome<bool>>(caller);
    const auto posted = state.sessionWorker.post([guard = WorkerRpcGuard<bool>(std::move(completion)), session = state.session, endpoint = state.endpoint, message = std::move(message), stop](ruvia::web_worker_context&) mutable -> ruvia::task<void> {
        WorkerRpcOutcome<bool> out;
        try {
            out.value.emplace(co_await session->forward(std::move(endpoint), std::move(message), std::move(stop)));
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
ruvia::task<void> writeTerminalChannel(Connection& socket, TerminalChannelState& state, ruvia::task_scope& scope) {
    try {
        while (!scope.stop_requested()) {
            auto delivery = co_await state.endpoint->receive(scope.token());
            if (scope.stop_requested()) {
                break;
            }
            if (!delivery || state.session->closed()) {
                if (!state.session->closed()) {
                    closeTerminalChannelSession(state, TerminalCloseReason::kQueueClosed);
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
                ready.mutable_channel_ready()->set_purpose(wire::SSH_STREAM);
                auto bytes = protocol::encodeMessage(ready);
                if (!bytes) {
                    throw std::runtime_error("Invalid terminal channel readiness");
                }
                co_await socket.binary(*bytes);
            } else {
                co_await socket.binary(delivery->bytes());
            }
        }
    } catch (const std::exception&) {
        closeTerminalChannelSession(state, TerminalCloseReason::kWriteFailed);
        socket.abort();
    }
}

template <typename Context, typename Connection>
ruvia::task<void> watchTerminalChannel(Context& context, Connection& socket, TerminalChannelState& state, ruvia::task_scope& scope) {
    auto nextAuthorization = std::chrono::steady_clock::now();
    try {
        while (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(1), scope.token()) == ruvia::sleep_status::elapsed) {
            if ((!state.session || !state.session->ready()) && std::chrono::steady_clock::now() >= state.deadline) {
                if (state.session) {
                    closeTerminalChannelSession(state, TerminalCloseReason::kSetupTimeout);
                }
                socket.abort();
                co_return;
            }
            if (state.session && state.session->closed()) {
                socket.abort();
                co_return;
            }
            if (state.owner && std::chrono::steady_clock::now() >= nextAuthorization) {
                auto& localRuntime = context.template worker_state<ServerRuntime>();
                const bool authorized = co_await localRuntime.sessions.authorized(context.db(), state.authorization->sessionId);
                if (!authorized) {
                    (void)co_await localRuntime.sessions.close(context.db(), state.authorization->userId, state.authorization->sessionId, SessionEnd::kPermissionRevoked);
                    closeTerminalChannelSession(state, TerminalCloseReason::kPermissionRevoked);
                    socket.abort();
                    co_return;
                }
                nextAuthorization = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
        }
    } catch (const std::exception&) {
        if (state.session) {
            closeTerminalChannelSession(state, TerminalCloseReason::kAuthorizationCheckFailed);
        }
        socket.abort();
    }
}

template <typename Context, typename Connection>
ruvia::task<void> exchangeTerminalChannel(Context& context, Connection& socket, protocol::ChannelSource source, TerminalChannelState& state, ruvia::task_scope& scope) {
    auto incoming = co_await socket.read();
    if (!incoming || !incoming->binary() || incoming->payload().size() > protocol::kMaximumControlBytes) {
        co_return;
    }
    auto open = protocol::decodeMessage(incoming->payload());
    incoming.reset();
    if (!open || !open->has_channel_open() || open->sequence() != 1 || open->generation() || open->frame_id() || open->base_frame_id() ||
        open->channel_open().purpose() != wire::SSH_STREAM) {
        co_return;
    }
    auto& runtime = context.template worker_state<ServerRuntime>();
    if (source == protocol::ChannelSource::kBrowser) {
        state.authorization = co_await runtime.sessions.consumeBrowser(context.db(), context.template request_state<UserIdentity>(), open->session_id(), open->channel_id(), wire::SSH_STREAM, open->channel_open().ticket());
    } else {
        state.authorization = co_await runtime.sessions.consumeAgent(context.db(), context.template request_state<DeviceIdentity>(), open->session_id(), open->channel_id(), wire::SSH_STREAM, open->channel_open().ticket());
    }
    open->Clear();
    if (!state.authorization || scope.stop_requested()) {
        co_return;
    }
    state.sessionWorker = resolveWebWorker(state.authorization->sessionId);
    const auto attachLocal = [auth = *state.authorization, source, ioWorker = context.worker()](ServerRuntime& targetRuntime, const ruvia::worker_ref& ownerWorker) {
        auto found = targetRuntime.terminalSessions.find(auth.sessionId);
        std::shared_ptr<TerminalSession> session;
        bool owner = false;
        if (found == targetRuntime.terminalSessions.end()) {
            session = std::make_shared<TerminalSession>(ownerWorker);
            targetRuntime.terminalSessions.emplace(auth.sessionId, session);
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
        using Attached = std::tuple<std::shared_ptr<TerminalSession>, std::shared_ptr<TerminalEndpoint>, bool>;
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
        closeTerminalChannelSession(state, TerminalCloseReason::kAttachRejected);
        co_return;
    }
    state.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    scope.spawn(writeTerminalChannel(socket, state, scope));
    while (true) {
        incoming = co_await socket.read();
        if (!incoming) {
            closeTerminalChannelSession(state, TerminalCloseReason::kPeerClosed);
            break;
        }
        if (!incoming->binary()) {
            closeTerminalChannelSession(state, TerminalCloseReason::kPeerNonBinary);
            break;
        }
        auto message = protocol::decodeMessage(incoming->payload());
        if (!message) {
            closeTerminalChannelSession(state, TerminalCloseReason::kMalformedMessage);
            break;
        }
        if (!(co_await forwardToTerminalSession(context.worker(), state, std::move(*message), scope.token()))) {
            break;
        }
    }
}
} // namespace

template <typename Context, typename Connection>
ruvia::task<void> runTerminalChannel(Context& context, Connection& socket, protocol::ChannelSource source) {
    TerminalChannelState state;
    state.source = source;
    auto worker = context.worker();
    const auto scopeWorker = worker;
    ruvia::task_scope scope(scopeWorker);
    bool scopeJoined = false;
    std::exception_ptr scopeFailure;
    try {
        scope.spawn(watchTerminalChannel(context, socket, state, scope));
        try {
            co_await exchangeTerminalChannel(context, socket, source, state, scope);
        } catch (const std::exception&) {
            if (state.session) {
                closeTerminalChannelSession(state, TerminalCloseReason::kReadFailed);
            }
        }
        if (state.session) {
            closeTerminalChannelSession(state, TerminalCloseReason::kPeerClosed);
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
            }
            if (state.owner) {
                if (!state.sessionWorker.valid() || state.sessionWorker.is_current()) {
                    runtime.terminalSessions.erase(state.authorization->sessionId);
                } else {
                    (void)state.sessionWorker.post([sessionId = state.authorization->sessionId](ruvia::web_worker_context& ctx) -> ruvia::task<void> {
                        ctx.worker_state<ServerRuntime>().terminalSessions.erase(sessionId);
                        co_return;
                    });
                }
                sendDeviceSessionClose(runtime, state.authorization->deviceId, state.authorization->sessionId, "channel_closed");
            }
        }
    } catch (...) {
        scopeFailure = std::current_exception();
        if (state.session) {
            closeTerminalChannelSession(state, TerminalCloseReason::kReadFailed);
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
} // namespace ultraterimal::server
