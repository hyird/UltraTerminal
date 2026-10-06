#include "control.runtime.h"

#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/timer.h>

#include "config/version.config.h"
#include "connection.transport.h"
#include "features/desktop/session.runtime.h"
#include "features/logging/log.runtime.h"
#include "platform/windows/capability.transport.h"
#include "platform/windows/credential.transport.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/system_restart.transport.h"
#include "platform/windows/update.transport.h"
#include "platform/windows/windows_login.transport.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
namespace {
wire::Envelope makeHello(const ConnectionConfig& config, bool desktop = false) {
    wire::Envelope hello;
    hello.set_protocol_version(protocol::kVersion);
    hello.set_sequence(1);
    hello.mutable_hello()->set_device_id(config.deviceId);
    hello.mutable_hello()->set_agent_version(kAgentVersion);
    const auto capabilities = detectCapabilities();
    auto* reported = hello.mutable_hello()->mutable_capabilities();
    reported->set_windows_login_settings(true);
    reported->set_agent_update(nativeSystemRestartPlatform().isSessionZeroLocalSystem());
    reported->set_system_restart(nativeSystemRestartPlatform().isSessionZeroLocalSystem());
    reported->set_boot_time_unix_seconds(systemBootTimeUnixSeconds());
    reported->set_conpty(capabilities.conpty);
    reported->set_display_count(capabilities.attachedDisplays);
    const auto selected = currentDesktopSession();
    const bool desktopAvailable = desktop && selected && selected != 0xffffffff &&
                                  (!capabilities.processSession || capabilities.attachedDisplays);
    reported->set_capture_available(desktopAvailable);
    reported->set_h264_software(desktopAvailable);
    reported->set_desktop_input_contract(2);
    return hello;
}
std::string encodeControl(const wire::Envelope& message) {
    auto bytes = protocol::encodeMessage(message);
    if (!bytes || bytes->size() > protocol::kMaximumControlBytes) {
        throw std::runtime_error("Cannot encode control message");
    }
    return std::move(*bytes);
}
} // namespace

asio::awaitable<ControlCheck> checkControlConnection(ruvia::execution_handle loop, const ConnectionConfig& config) {
    DeviceTransport transport(loop, config, protocol::kMaximumControlBytes);
    co_await transport.connect(loadCredential(config.credentialFile));
    co_await transport.send(encodeControl(makeHello(config)));
    auto responseBytes = co_await transport.receive();
    if (responseBytes.size() > protocol::kMaximumControlBytes) {
        throw std::runtime_error("Control response exceeds limit");
    }
    auto response = protocol::decodeMessage(responseBytes);
    if (!response || !response->has_welcome()) {
        throw std::runtime_error("Gateway did not accept the control handshake");
    }
    const auto heartbeatSeconds = response->welcome().heartbeat_seconds();
    const auto sequence = response->sequence();
    wire::Envelope heartbeat;
    heartbeat.set_protocol_version(protocol::kVersion);
    heartbeat.set_sequence(2);
    heartbeat.mutable_heartbeat()->set_monotonic_milliseconds(static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    auto heartbeatBytes = protocol::encodeMessage(heartbeat);
    if (!heartbeatBytes) {
        throw std::runtime_error("Cannot encode heartbeat");
    }
    co_await transport.send(std::move(*heartbeatBytes));
    responseBytes = co_await transport.receive();
    if (responseBytes.size() > protocol::kMaximumControlBytes) {
        throw std::runtime_error("Control response exceeds limit");
    }
    response = protocol::decodeMessage(responseBytes);
    if (!response || !response->has_heartbeat() || response->sequence() <= sequence) {
        throw std::runtime_error("Invalid or replayed heartbeat acknowledgment");
    }
    co_await transport.close();
    co_return ControlCheck{heartbeatSeconds};
}
struct ControlRuntime::State : std::enable_shared_from_this<State> {
    ruvia::execution_handle loop;
    ruvia::blocking_pool settingsPool{{.thread_count_ = 1, .queue_capacity_ = 1}};
    ruvia::stop_source stopSource;
    asio::any_io_executor executor;
    ConnectionConfig config;
    asio::steady_timer writeReady, heartbeatTimer, deadline, backoff, sessionsDone, logTimer;
    std::filesystem::path desktopExecutable;
    ConsoleSessionEvents* consoleEvents;
    struct SessionEntry {
        std::shared_ptr<DesktopSession> runtime;
        bool writable;
    };
    std::map<std::string, SessionEntry> sessions;
    std::string credential;
    std::unique_ptr<DeviceTransport> transport;
    std::deque<std::string> outbound;
    std::size_t queuedBytes = 0;
    std::uint64_t outgoingSequence = 1, incomingSequence = 0, deadlineGeneration = 0;
    ControlStatistics statistics;
    std::mt19937 random{std::random_device{}()};
    AgentLogBuffer logs{std::to_string(random()) + "-" + std::to_string(random())};
    bool logsSupported = false, logPending = false, connectionFailureLogged = false;
    bool updating = false;
    bool started = false, stopping = false, ended = false, heartbeatPending = false;
    unsigned heartbeatSeconds = 0;
    ~State() { settingsPool.join(); }

    State(ruvia::execution_handle owner, ConnectionConfig settings, std::filesystem::path executable, ConsoleSessionEvents* events)
        : loop(owner), executor(owner.executor()), config(std::move(settings)), writeReady(executor), heartbeatTimer(executor), deadline(executor), backoff(executor),
          sessionsDone(executor), logTimer(executor), desktopExecutable(std::move(executable)), consoleEvents(events) {
        if (!desktopExecutable.empty() && !desktopExecutable.is_absolute()) {
            throw std::invalid_argument("Desktop executable must be absolute");
        }
    }

    void log(wire::AgentLog::Event event, std::string sessionId = {}, std::uint32_t exitCode = 0, bool forcedExit = false, const wire::DesktopFault* fault = nullptr) {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        logs.append(event, static_cast<std::uint64_t>(now), std::move(sessionId), exitCode, forcedExit, fault);
    }
    void connectionFailure(wire::AgentLog::Event event) {
        if (!stopping && !connectionFailureLogged) {
            log(event);
            connectionFailureLogged = true;
        }
    }

    void disconnect() {
        ended = true;
        statistics.connected = false;
        ++deadlineGeneration;
        deadline.cancel();
        writeReady.cancel();
        heartbeatTimer.cancel();
        logTimer.cancel();
        if (transport) {
            transport->cancel();
        }
        for (auto& [id, session] : sessions) {
            session.runtime->stop();
        }
    }
    void stop() {
        stopping = true;
        disconnect();
        backoff.cancel();
    }
    void armDeadline(std::chrono::seconds duration) {
        const auto generation = ++deadlineGeneration;
        deadline.expires_after(duration);
        deadline.async_wait([weak = weak_from_this(), generation](std::error_code error) {
            if (const auto self = weak.lock(); self && !error && generation == self->deadlineGeneration) {
                self->connectionFailure(self->statistics.connected ? wire::AgentLog::HEARTBEAT_TIMEOUT : wire::AgentLog::CONNECT_FAILED);
                self->disconnect();
            }
        });
    }
    void enqueue(wire::Envelope message) {
        if (outgoingSequence == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("Control sequence exhausted");
        }
        message.set_protocol_version(protocol::kVersion);
        message.set_sequence(++outgoingSequence);
        auto bytes = encodeControl(message);
        // The in-flight write owns at most one additional control message.
        if (outbound.size() >= 32 || queuedBytes + bytes.size() > 256 * 1024) {
            throw std::runtime_error("Control send queue exhausted");
        }
        queuedBytes += bytes.size();
        outbound.push_back(std::move(bytes));
        writeReady.cancel();
    }
    asio::awaitable<void> writeLoop() {
        try {
            while (!ended) {
                if (outbound.empty()) {
                    writeReady.expires_at(std::chrono::steady_clock::time_point::max());
                    std::error_code error;
                    co_await writeReady.async_wait(asio::redirect_error(asio::use_awaitable, error));
                    continue;
                }
                auto bytes = std::move(outbound.front());
                outbound.pop_front();
                queuedBytes -= bytes.size();
                co_await transport->send(std::move(bytes));
            }
        } catch (...) {
            connectionFailure(wire::AgentLog::WRITE_FAILED);
            disconnect();
            throw;
        }
    }
    asio::awaitable<void> logLoop() {
        while (!ended) {
            // The durable update result is a recovery record, not a local log.
            try {
                const auto result = (co_await ruvia::as_awaitable(ruvia::run_blocking(settingsPool, loop, [root = desktopExecutable.parent_path()] { return takeAgentUpdateResult(root); }))).value();
                if (result) {
                    updating = false;
                    log(*result == 0 || *result == 3010 ? wire::AgentLog::UPDATE_COMPLETED : wire::AgentLog::UPDATE_FAILED, {}, *result);
                }
            } catch (const std::exception&) { /* Retry if the bounded settings pool is busy. */
            }
            // Never fill the control queue with diagnostics. One acknowledged batch at a time.
            if (logsSupported && !logPending && outbound.empty()) {
                if (const auto* batch = logs.next()) {
                    wire::Envelope message;
                    *message.mutable_agent_logs() = *batch;
                    enqueue(std::move(message));
                    logPending = true;
                }
            }
            logTimer.expires_after(std::chrono::seconds(1));
            std::error_code error;
            co_await logTimer.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
    }
    asio::awaitable<void> heartbeatLoop() {
        try {
            while (!ended) {
                if (!heartbeatPending) {
                    wire::Envelope message;
                    message.mutable_heartbeat()->set_monotonic_milliseconds(static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
                    enqueue(std::move(message));
                    heartbeatPending = true;
                    armDeadline(std::chrono::seconds(heartbeatSeconds * 2));
                }
                heartbeatTimer.expires_after(std::chrono::seconds(heartbeatSeconds));
                co_await heartbeatTimer.async_wait(asio::use_awaitable);
            }
        } catch (...) {
            disconnect();
            throw;
        }
    }
    asio::awaitable<void> readLoop() {
        try {
            while (!ended) {
                const auto bytes = co_await transport->receive();
                auto message = protocol::decodeMessage(bytes);
                if (!message || message->sequence() <= incomingSequence) {
                    throw std::runtime_error("Invalid or replayed control message");
                }
                incomingSequence = message->sequence();
                if (message->has_agent_update()) {
                    if (!updating) {
                        bool launched = false;
                        try {
                            launched = (co_await ruvia::as_awaitable(ruvia::run_blocking(settingsPool, loop, [path = desktopExecutable, update = message->agent_update()] { return launchAgentUpdate(path, update); }))).value();
                        } catch (const std::exception&) {
                        }
                        updating = launched;
                        log(launched ? wire::AgentLog::UPDATE_STARTED : wire::AgentLog::UPDATE_FAILED);
                    }
                } else if (message->has_agent_log_ack()) {
                    if (!logsSupported || !logPending || !logs.acknowledge(message->agent_log_ack().sequence())) {
                        throw std::runtime_error("Unexpected log acknowledgment");
                    }
                    logPending = false;
                } else if (message->has_windows_login_request()) {
                    auto operation = ruvia::run_blocking(settingsPool, loop,
                                                         [path = config.credentialFile.parent_path() / "windows_login.credential", request = std::move(*message->mutable_windows_login_request())]() mutable {
                                                             return windowsLoginSettings(path, std::move(request));
                                                         });
                    auto result = (co_await ruvia::as_awaitable(std::move(operation))).value();
                    if (!ended) {
                        wire::Envelope response;
                        *response.mutable_windows_login_result() = std::move(result);
                        enqueue(std::move(response));
                    }
                } else if (message->has_windows_restart_request()) {
                    if (!message->session_id().empty() || !message->channel_id().empty()) {
                        throw std::runtime_error("Restart request is not permitted on a session channel");
                    }
                    auto operation = ruvia::run_blocking(settingsPool, loop,
                                                         [deviceId = config.deviceId,
                                                          request = std::move(*message->mutable_windows_restart_request())]() mutable {
                                                             const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                                                             std::chrono::system_clock::now().time_since_epoch())
                                                                                                             .count());
                                                             return handleWindowsRestart(deviceId, request, nativeSystemRestartPlatform(), now);
                                                         });
                    auto result = (co_await ruvia::as_awaitable(std::move(operation))).value();
                    if (!ended) {
                        wire::Envelope response;
                        *response.mutable_windows_restart_result() = std::move(result);
                        enqueue(std::move(response));
                    }
                } else if (message->has_heartbeat()) {
                    if (!heartbeatPending || !message->session_id().empty() || !message->channel_id().empty()) {
                        throw std::runtime_error("Unexpected heartbeat acknowledgment");
                    }
                    heartbeatPending = false;
                    ++deadlineGeneration;
                    deadline.cancel();
                    ++statistics.acknowledgments;
                } else if (message->has_session_open()) {
                    openSession(*message);
                } else if (message->has_session_close()) {
                    if (auto found = sessions.find(message->session_id()); found != sessions.end()) {
                        found->second.runtime->stop();
                    }
                } else if (!message->has_session_close()) {
                    throw std::runtime_error("Unexpected control message type");
                }
            }
        } catch (...) {
            connectionFailure(wire::AgentLog::READ_FAILED);
            disconnect();
            throw;
        }
    }
    void openSession(const wire::Envelope& message) {
        if (sessions.contains(message.session_id())) {
            throw std::runtime_error("Duplicate active desktop session");
        }
        const auto selected = currentDesktopSession();
        const bool writable = message.session_open().write_control();
        const char* rejection = desktopExecutable.empty() || message.session_open().kind() != wire::DESKTOP
                                    ? "capability_unavailable"
                                : !selected || selected == 0xffffffff ? "desktop_unavailable"
                                                                      : nullptr;
        if (rejection) {
            log(wire::AgentLog::DESKTOP_REJECTED, message.session_id());
            wire::Envelope response;
            response.set_session_id(message.session_id());
            response.mutable_session_error()->set_code(rejection);
            response.mutable_session_error()->set_message("Requested session cannot be started");
            enqueue(std::move(response));
            ++statistics.rejectedSessions;
            return;
        }
        for (auto& [id, session] : sessions) {
            session.runtime->stop();
        }
        sessions.clear();
        auto runtime = std::make_shared<DesktopSession>(loop, config, message, desktopExecutable, selected, consoleEvents);
        sessions.emplace(message.session_id(), SessionEntry{runtime, writable});
        ++statistics.startedSessions;
        log(wire::AgentLog::DESKTOP_STARTED, message.session_id());
        try {
            asio::co_spawn(executor, runtime->run(credential), [self = shared_from_this(), runtime, id = message.session_id()](std::exception_ptr failure, DesktopSessionResult result) {
                self->sessions.erase(id);
                ++self->statistics.completedSessions;
                self->sessionsDone.cancel();
                self->log(failure || result.terminal.has_session_error() ? wire::AgentLog::DESKTOP_FAILED : wire::AgentLog::DESKTOP_ENDED, id, result.exitCode, result.forcedExit, result.fault ? &*result.fault : nullptr);
                if (!self->ended) {
                    try {
                        if (failure) {
                            result.terminal.Clear();
                            result.terminal.set_session_id(id);
                            result.terminal.mutable_session_error()->set_code("desktop_failed");
                            result.terminal.mutable_session_error()->set_message("Desktop session failed");
                        }
                        self->enqueue(std::move(result.terminal));
                    } catch (...) {
                        self->disconnect();
                    }
                }
            });
        } catch (...) {
            sessions.erase(message.session_id());
            runtime->stop();
            throw;
        }
    }
    asio::awaitable<void> joinSessions() {
        while (!sessions.empty()) {
            sessionsDone.expires_at(std::chrono::steady_clock::time_point::max());
            std::error_code error;
            co_await sessionsDone.async_wait(asio::redirect_error(asio::use_awaitable, error));
        }
    }
    asio::awaitable<void> connect() {
        transport = std::make_unique<DeviceTransport>(loop, config, protocol::kMaximumControlBytes);
        credential = loadCredential(config.credentialFile);
        co_await transport->connect(credential);
        armDeadline(config.connectTimeout);
        co_await transport->send(encodeControl(makeHello(config, !desktopExecutable.empty())));
        const auto bytes = co_await transport->receive();
        const auto welcome = protocol::decodeMessage(bytes);
        if (!welcome || !welcome->has_welcome() || !welcome->session_id().empty() || !welcome->channel_id().empty()) {
            throw std::runtime_error("Gateway did not accept the control handshake");
        }
        ++deadlineGeneration;
        deadline.cancel();
        if (stopping || ended) {
            throw std::runtime_error("Control connection was cancelled");
        }
        incomingSequence = welcome->sequence();
        heartbeatSeconds = welcome->welcome().heartbeat_seconds();
        logsSupported = welcome->welcome().agent_logs();
        statistics.connected = true;
        ++statistics.connections;
        log(wire::AgentLog::CONNECTED);
    }
    static asio::awaitable<void> runConnections(std::shared_ptr<State> self) {
        if (self->started) {
            throw std::logic_error("Control runtime can only run once");
        }
        self->started = true;
        self->log(wire::AgentLog::STARTED);
        // Missing or unreadable local identity is a startup error, not a retry loop.
        (void)loadCredential(self->config.credentialFile);
        auto retry = std::chrono::milliseconds(500);
        while (!self->stopping) {
            self->ended = false;
            self->outgoingSequence = 1;
            self->incomingSequence = 0;
            self->heartbeatPending = false;
            self->logPending = false;
            self->connectionFailureLogged = false;
            const auto startedAt = std::chrono::steady_clock::now();
            std::exception_ptr fatal;
            ++self->statistics.attempts;
            try {
                co_await self->connect();
                using namespace asio::experimental::awaitable_operators;
                co_await (self->writeLoop() && self->readLoop() && self->heartbeatLoop() && self->logLoop());
            } catch (const std::bad_alloc&) {
                fatal = std::current_exception();
                self->disconnect();
                self->stopping = true;
            } catch (const std::exception&) {
                // No peer-controlled exception strings or credentials enter ordinary logs.
                self->connectionFailure(wire::AgentLog::CONNECT_FAILED);
                self->disconnect();
            }
            self->disconnect();
            co_await self->joinSessions();
            // All child tasks have joined before resetting transport and queued storage.
            self->transport.reset();
            self->outbound.clear();
            self->queuedBytes = 0;
            self->credential.clear();
            if (fatal) {
                std::rethrow_exception(fatal);
            }
            if (self->stopping) {
                break;
            }
            if (std::chrono::steady_clock::now() - startedAt >= std::chrono::seconds(30)) {
                retry = std::chrono::milliseconds(500);
            }
            std::uniform_int_distribution<long long> jitter(retry.count() / 2, retry.count());
            self->backoff.expires_after(std::chrono::milliseconds(jitter(self->random)));
            std::error_code error;
            co_await self->backoff.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (error && !self->stopping) {
                throw std::system_error(error);
            }
            retry = std::min(retry * 2, std::chrono::milliseconds(30000));
        }
    }
    asio::awaitable<void> watchStop() {
        auto worker = loop;
        while (!stopSource.stop_requested()) {
            (void)co_await ruvia::as_awaitable(ruvia::sleep_for(worker, std::chrono::hours(24), stopSource.token()));
        }
        stop();
    }
    static asio::awaitable<void> runAndStop(std::shared_ptr<State> self) {
        try {
            co_await runConnections(self);
        } catch (...) {
            self->stopSource.request_stop();
            throw;
        }
        self->stopSource.request_stop();
    }
    static asio::awaitable<void> runOwned(std::shared_ptr<State> self) {
        using namespace asio::experimental::awaitable_operators;
        co_await (runAndStop(self) && self->watchStop());
    }
};
ControlRuntime::ControlRuntime(ruvia::execution_handle loop, ConnectionConfig config, std::filesystem::path desktopExecutable, ConsoleSessionEvents* consoleEvents)
    : state_(std::make_shared<State>(std::move(loop), std::move(config), std::move(desktopExecutable), consoleEvents)) {}
ControlRuntime::~ControlRuntime() { stop(); }
asio::awaitable<void> ControlRuntime::run() { return State::runOwned(state_); }
void ControlRuntime::stop() {
    state_->stopSource.request_stop();
}
ControlStatistics ControlRuntime::statistics() const { return state_->statistics; }
} // namespace ultraterimal::agent
