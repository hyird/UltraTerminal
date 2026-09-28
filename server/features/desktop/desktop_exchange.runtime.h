#pragma once

#include <chrono>
#include <optional>

#include <ruvia/core/TaskScope.h>
#include <ruvia/core/Timer.h>

#include "config/server.runtime.h"
#include "modules/identity/identity.service.h"

namespace ultraterimal::server {
namespace {
using Clock = std::chrono::steady_clock;
struct ChannelState {
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    protocol::ChannelSource source = protocol::ChannelSource::kBrowser;
    std::optional<ChannelAuthorization> authorization;
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
template <typename Connection>
ruvia::Task<void> writeChannel(Connection& socket, ChannelState& state, ruvia::TaskScope& scope) {
    try {
        while (!scope.stopRequested()) {
            auto delivery = co_await state.endpoint->receive(scope.stopToken());
            if (scope.stopRequested()) {
                break;
            }
            if (!delivery || state.session->closed()) {
                if (!state.session->closed()) {
                    state.session->close(DesktopCloseReason::kQueueClosed, state.origin());
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
                auto bytes = protocol::encodeMessage(ready);
                if (!bytes) {
                    throw std::runtime_error("Invalid channel readiness");
                }
                co_await socket.binary(*bytes);
            } else {
                co_await socket.binary(delivery->bytes());
            }
        }
    } catch (const std::exception&) {
        state.session->close(DesktopCloseReason::kWriteFailed, state.origin());
        socket.abort();
    }
}
template <typename Context, typename Connection>
ruvia::Task<void> watchChannel(Context& context, Connection& socket, ChannelState& state, ruvia::TaskScope& scope) {
    auto nextAuthorization = Clock::now();
    auto nextHealth = Clock::now() + std::chrono::seconds(30);
    try {
        while (co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1), scope.stopToken()) == ruvia::TimerSleepResult::kElapsed) {
            if ((!state.session || !state.session->ready()) && Clock::now() >= state.deadline) {
                if (state.session) {
                    state.session->close(DesktopCloseReason::kSetupTimeout, state.origin());
                }
                socket.abort();
                co_return;
            }
            if (state.session && state.session->closed()) {
                socket.abort();
                co_return;
            }
            if (state.owner && state.session && Clock::now() >= nextHealth) {
                state.session->logHealth();
                nextHealth = Clock::now() + std::chrono::seconds(30);
            }
            if (state.owner && Clock::now() >= nextAuthorization) {
                const bool authorized = co_await context.runBlocking([service = context.template workerState<ServerRuntime>().sessions, id = state.authorization->sessionId] { return service.authorized(id); });
                if (!authorized) {
                    co_await context.runBlocking([service = context.template workerState<ServerRuntime>().sessions, auth = *state.authorization] { service.close(auth.userId, auth.sessionId, SessionEnd::kPermissionRevoked); });
                    state.session->close(DesktopCloseReason::kPermissionRevoked, state.origin());
                    socket.abort();
                    co_return;
                }
                nextAuthorization = Clock::now() + std::chrono::seconds(5);
            }
        }
    } catch (const std::exception&) {
        if (state.session) {
            state.session->close(DesktopCloseReason::kAuthorizationCheckFailed, state.origin());
        }
        socket.abort();
    }
}
template <typename Context, typename Connection>
ruvia::Task<void> exchangeChannel(Context& context, Connection& socket, protocol::ChannelSource source, ChannelState& state, ruvia::TaskScope& scope, std::optional<wire::ChannelPurpose> expectedPurpose) {
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
    if (source == protocol::ChannelSource::kBrowser) {
        state.authorization = co_await context.runBlocking([service = context.template workerState<ServerRuntime>().sessions, user = context.template requestState<UserIdentity>(), message = *open] {
            return service.consumeBrowser(user.id, message.session_id(), message.channel_id(), message.channel_open().purpose(), message.channel_open().ticket());
        });
    } else {
        state.authorization = co_await context.runBlocking([service = context.template workerState<ServerRuntime>().sessions, device = context.template requestState<DeviceIdentity>(), message = *open] {
            return service.consumeAgent(device, message.session_id(), message.channel_id(), message.channel_open().purpose(), message.channel_open().ticket());
        });
    }
    open->Clear(); // The live runtime does not retain bearer tickets.
    if (!state.authorization || scope.stopRequested()) {
        co_return;
    }
    auto& runtime = context.template workerState<ServerRuntime>();
    auto found = runtime.desktopSessions.find(state.authorization->sessionId);
    if (found == runtime.desktopSessions.end()) {
        state.session = std::make_shared<DesktopSession>(context.worker(), runtime.desktopBudget);
        runtime.desktopSessions.emplace(state.authorization->sessionId, state.session);
        state.owner = true;
    } else {
        state.session = found->second;
    }
    state.endpoint = state.session->attach(*state.authorization, source);
    if (!state.endpoint) {
        state.session->close(DesktopCloseReason::kAttachRejected, state.origin());
        co_return;
    }
    state.deadline = Clock::now() + std::chrono::seconds(60);
    scope.spawn(writeChannel(socket, state, scope));
    while (true) {
        incoming = co_await socket.read();
        if (!incoming) {
            state.session->close(DesktopCloseReason::kPeerClosed, state.origin());
            break;
        }
        if (!incoming->binary()) {
            state.session->close(DesktopCloseReason::kPeerNonBinary, state.origin());
            break;
        }
        const auto message = protocol::decodeMessage(incoming->payload());
        if (!message) {
            state.session->close(DesktopCloseReason::kMalformedMessage, state.origin());
            break;
        }
        if (!(co_await state.session->forward(state.endpoint, *message, scope.stopToken()))) {
            break;
        }
    }
}
template <typename Context, typename Connection>
ruvia::Task<void> channel(Context& context, Connection& socket, protocol::ChannelSource source, std::optional<wire::ChannelPurpose> expectedPurpose = std::nullopt) {
    ChannelState state;
    state.source = source;
    auto worker = context.worker();
    ruvia::TaskScope scope(worker);
    scope.spawn(watchChannel(context, socket, state, scope));
    try {
        co_await exchangeChannel(context, socket, source, state, scope, expectedPurpose);
    } catch (const std::exception&) {
        if (state.session) {
            state.session->close(DesktopCloseReason::kReadFailed, state.origin());
        }
    }
    if (state.session) {
        state.session->close(DesktopCloseReason::kPeerClosed, state.origin());
    }
    socket.abort();
    scope.requestStop();
    co_await scope.join();
    if (state.authorization) {
        auto& runtime = context.template workerState<ServerRuntime>();
        try {
            co_await context.runBlocking([service = runtime.sessions, auth = *state.authorization] { service.close(auth.userId, auth.sessionId, SessionEnd::kFailure); });
        } catch (const std::exception&) {
            // A later permission check, reconnect or gateway recovery closes the persisted session.
        }
        if (state.owner) {
            runtime.desktopSessions.erase(state.authorization->sessionId);
            auto control = runtime.controlConnections.find(state.authorization->deviceId);
            if (control != runtime.controlConnections.end()) {
                wire::Envelope close;
                close.set_session_id(state.authorization->sessionId);
                close.mutable_session_close()->set_reason("channel_closed");
                if (!control->second->enqueue(std::move(close))) {
                    control->second->close();
                }
            }
        }
    }
}
} // namespace
template <typename Context, typename Connection>
ruvia::Task<void> runDesktopChannel(Context& context, Connection& socket, protocol::ChannelSource source, std::optional<wire::ChannelPurpose> expectedPurpose = std::nullopt) {
    co_await channel(context, socket, source, expectedPurpose);
}
} // namespace ultraterimal::server
