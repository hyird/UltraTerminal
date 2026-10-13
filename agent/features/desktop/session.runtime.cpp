#include "session.runtime.h"

#include <algorithm>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/blocking_pool.h>

#include "bridge.runtime.h"
#include "desktop.transport.h"
#include "handoff.runtime.h"
#include "platform/windows/console_session.transport.h"
#include "platform/windows/desktop_failure.error.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/login_broker.transport.h"
#include "platform/windows/pipe.transport.h"
#include "relay.runtime.h"

namespace ultraterimal::agent {
namespace {
struct Resources {
    std::unique_ptr<NamedPipeTransport> pipe;
    std::unique_ptr<DesktopProcess> child;
};
bool validSession(std::uint32_t session) { return session && session != 0xffffffff; }
} // namespace
struct DesktopSession::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    ruvia::blocking_pool systemPool{{.thread_count_ = 1, .queue_capacity_ = 2}};
    DesktopTransport remote;
    DesktopRelay relay;
    asio::steady_timer grace, transition;
    std::unique_ptr<ConsoleSessionSubscription> console;
    std::filesystem::path executable, loginCredential;
    std::unique_ptr<WindowsLoginBroker> login;
    std::string session;
    DesktopHandoffState binding;
    bool writeControl;
    std::uint32_t desktopInputContract;
    bool started = false, stopping = false, ended = false, joined = false;
    Resources resources;
    DesktopSessionResult result;
    DesktopHandoffBudget handoffs;
    std::exception_ptr bridgeFailure;

    State(ruvia::event_loop owner, ConnectionConfig config, wire::Envelope grant,
          std::filesystem::path path, std::uint32_t selected, ConsoleSessionEvents* events)
        : loop(owner), remote(owner, config, grant), relay(owner, remote), grace(owner.executor()), transition(owner.executor()),
          executable(std::move(path)), loginCredential(config.credentialFile.parent_path() / "windows_login.credential"), session(grant.session_id()), binding(selected),
          writeControl(grant.session_open().write_control()), desktopInputContract(grant.session_open().desktop_input_contract()) {
        if ((writeControl && desktopInputContract != 2) || (!writeControl && desktopInputContract != 0)) {
            throw std::invalid_argument("Desktop input contract is unsupported");
        }
        if (!validSession(selected)) {
            throw std::invalid_argument("Invalid Windows desktop session");
        }
        if (events) {
            console = std::make_unique<ConsoleSessionSubscription>(owner.executor(), *events);
        }
    }
    ~State() { systemPool.join(); }
    void detachWorker() {
        relay.detach();
        if (login) {
            login->stop();
        }
        if (resources.pipe) {
            resources.pipe->cancel();
        }
    }
    void finish() {
        ended = true;
        detachWorker();
        relay.stop();
        transition.cancel();
        if (console) {
            console->cancel();
        }
    }
    void stop() {
        stopping = true;
        finish();
    }
    void terminalError(const char* code, const char* message) {
        result.terminal.Clear();
        result.terminal.mutable_session_error()->set_code(code);
        result.terminal.mutable_session_error()->set_message(message);
    }
    void rememberFailure(const char* stage, std::exception_ptr failure) {
        if (stopping || result.fault) {
            return;
        }
        auto point = desktopFailureStage(stage);
        try {
            std::rethrow_exception(failure);
        } catch (const DesktopFailure& error) {
            if (error.stage() != wire::DesktopFault::STAGE_UNKNOWN) {
                point = error.stage();
            }
        } catch (...) {
        }
        result.fault.emplace();
        result.fault->set_stage(point);
        result.fault->set_code(desktopFailureCode(failure, point));
        result.fault->set_windows_session_id(binding.session());
        result.fault->set_process_id(result.processId);
    }
    void fail(const char* stage, std::exception_ptr failure) {
        if (!ended) {
            rememberFailure(stage, failure);
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                reportDesktopFailure(stage, error.what(), session);
            } catch (...) {
                reportDesktopFailure(stage, "Unknown desktop exception", session);
            }
            terminalError("desktop_failed", "Desktop session could not continue");
        }
        finish();
    }
    asio::awaitable<void> observeConsole() {
        if (!console) {
            co_return;
        }
        try {
            while (!ended) {
                co_await console->wait();
                if (!ended && binding.observe(currentDesktopSession())) {
                    detachWorker();
                }
            }
        } catch (...) {
            if (!ended) {
                fail("session_bridge", std::current_exception());
            }
        }
    }
    asio::awaitable<void> channels() {
        try {
            co_await relay.run();
            if (!ended) {
                result.terminal = relay.terminal();
                finish();
            }
        } catch (...) {
            fail("session_bridge", std::current_exception());
        }
    }
    asio::awaitable<std::uint32_t> selectConsole() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto delay = std::chrono::milliseconds(50);
        std::uint32_t candidate = UINT32_MAX;
        while (!ended) {
            const auto selected = currentDesktopSession();
            if (validSession(selected) && selected == candidate) {
                co_return selected;
            }
            candidate = validSession(selected) ? selected : UINT32_MAX;
            if (std::chrono::steady_clock::now() >= deadline) {
                throw DesktopFailure(wire::DesktopFault::DESKTOP_UNAVAILABLE, "Console transition exceeded its deadline", wire::DesktopFault::WORKER_LAUNCH);
            }
            transition.expires_after(delay);
            std::error_code error;
            co_await transition.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (error && !ended) {
                throw std::system_error(error);
            }
            delay = validSession(selected) ? std::chrono::milliseconds(50) : std::min(delay * 2, std::chrono::milliseconds(500));
        }
        co_return 0;
    }
    asio::awaitable<void> bridge(std::shared_ptr<DesktopWorkerChannel> channel) {
        try {
            auto terminal = co_await bridgeDesktop(loop, systemPool, *channel, *resources.pipe, session, binding.session(), 0, writeControl, desktopInputContract);
            if (!ended) {
                result.terminal = std::move(terminal);
            }
        } catch (...) {
            bridgeFailure = std::current_exception();
            if (login) {
                login->stop();
            }
            throw;
        }
        if (login) {
            login->stop();
        }
    }
    asio::awaitable<std::uint32_t> reap() {
        if (!resources.child) {
            co_return 0;
        }
        joined = false;
        grace.expires_after(std::chrono::seconds(5));
        grace.async_wait([weak = weak_from_this()](std::error_code error) {
            if (auto state = weak.lock(); state && !error && !state->joined) {
                state->result.forcedExit = true;
                try {
                    state->resources.child->terminate();
                } catch (...) {
                    state->resources.child.reset();
                }
            }
        });
        const auto code = co_await resources.child->wait();
        joined = true;
        grace.cancel();
        co_return code;
    }
    asio::awaitable<void> workers() {
        // Cleanup must outlive cancellation of any sibling native/notification
        // task, including an outstanding blocking launch and process wait.
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        try {
            while (!ended) {
                const auto selected = co_await selectConsole();
                if (ended) {
                    break;
                }
                if (!binding.select(selected)) {
                    throw std::runtime_error("Invalid console selection");
                }
                bool workerStarted = false;
                result.processId = 0;
                std::exception_ptr failure;
                bridgeFailure = {};
                const char* stage = "worker_launch";
                try {
                    auto launch = ruvia::run_blocking(systemPool, loop.handle(),
                                                      [executor = loop.executor(), selected = binding.session(), path = executable] {
                                                          DesktopIdentity user(selected);
                                                          Resources resources;
                                                          resources.pipe = std::make_unique<NamedPipeTransport>(executor, user.pipeSid());
                                                          resources.child = std::make_unique<DesktopProcess>(executor, user, path, resources.pipe->name());
                                                          return resources;
                                                      });
                    resources = (co_await ruvia::as_awaitable(std::move(launch)));
                    result.processId = resources.child->id();
                    if (ended || binding.changed(currentDesktopSession())) {
                        throw std::runtime_error("Console changed during worker launch");
                    }
                    stage = "worker_pipe_pairing";
                    co_await resources.pipe->accept(result.processId, binding.session());
                    if (ended || binding.changed(currentDesktopSession())) {
                        throw std::runtime_error("Console changed during worker pairing");
                    }
                    stage = "login_broker";
                    if (writeControl) {
                        login = WindowsLoginBroker::create(loop, systemPool, loginCredential, binding.session());
                    }
                    stage = "session_bridge";
                    auto channel = relay.attach();
                    workerStarted = true;
                    if (login) {
                        using namespace asio::experimental::awaitable_operators;
                        co_await (bridge(channel) && login->run());
                    } else {
                        co_await bridge(channel);
                    }
                } catch (...) {
                    failure = bridgeFailure ? bridgeFailure : std::current_exception();
                }
                detachWorker();
                const auto code = co_await reap();
                const auto workerFault = protocol::desktopFailureFromExitCode(code, binding.session(), result.processId);
                // A concrete worker fault is not erased by a coincident console
                // event. Only teardown IPC errors or a clean OS exit are expected.
                const auto current = currentDesktopSession();
                const bool changed = binding.changed(current);
                bool independentFailure = false;
                if (failure) {
                    const auto failureCode = desktopFailureCode(failure, desktopFailureStage(stage));
                    independentFailure = failureCode == wire::DesktopFault::STARTUP_TIMEOUT || failureCode == wire::DesktopFault::PROTOCOL_REJECTED;
                }
                const bool replaceable = !independentFailure && !result.terminal.has_session_error() && !result.forcedExit && binding.replaceableExit(current, code, workerStarted, workerFault ? std::optional{workerFault->code()} : std::nullopt);
                if (!ended && changed && replaceable && handoffs.admit()) {
                    resources = {};
                    login.reset();
                    result.terminal.Clear();
                    continue;
                }
                if (!ended && failure) {
                    fail(stage, failure);
                } else if (!ended && changed) {
                    result.recordWorkerExit(code, binding.session());
                    fail("session_bridge", std::make_exception_ptr(DesktopFailure(wire::DesktopFault::DESKTOP_UNAVAILABLE, "Console handoff budget exhausted", wire::DesktopFault::WORKER_LAUNCH)));
                }
                result.recordWorkerExit(code, binding.session());
                if (result.hasIndependentFailure()) {
                    terminalError("desktop_failed", "Desktop worker reported an independent failure");
                }
                if (result.forcedExit) {
                    terminalError("desktop_exit_timeout", "Desktop process exceeded its exit deadline");
                }
                resources = {};
                login.reset();
                finish();
            }
        } catch (...) {
            fail("session_bridge", std::current_exception());
        }
        // A failed launch is owned until completion. Join any resulting child
        // even when selection or handoff itself fails.
        if (resources.child) {
            const auto code = co_await reap();
            result.recordWorkerExit(code, binding.session());
        }
        resources = {};
        login.reset();
    }
    static asio::awaitable<DesktopSessionResult> runOwned(std::shared_ptr<State> self, std::string credential) {
        if (self->started) {
            throw std::logic_error("Desktop session can only run once");
        }
        self->started = true;
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        try {
            if (!self->ended) {
                co_await self->remote.connect(std::move(credential));
                using namespace asio::experimental::awaitable_operators;
                co_await (self->workers() && self->channels() && self->observeConsole());
            }
        } catch (...) {
            self->fail("native_pairing", std::current_exception());
        }
        self->finish();
        if (self->stopping || self->result.terminal.has_session_close()) {
            self->result.applyCancellation();
        }
        self->result.terminal.set_protocol_version(protocol::kVersion);
        self->result.terminal.set_session_id(self->session);
        self->result.terminal.set_sequence(1);
        co_return std::move(self->result);
    }
};
DesktopSession::DesktopSession(ruvia::event_loop loop, ConnectionConfig config, wire::Envelope grant,
                               std::filesystem::path executable, std::uint32_t windowsSession, ConsoleSessionEvents* consoleEvents)
    : state_(std::make_shared<State>(std::move(loop), std::move(config), std::move(grant), std::move(executable), windowsSession, consoleEvents)) {}
DesktopSession::~DesktopSession() { stop(); }
asio::awaitable<DesktopSessionResult> DesktopSession::run(std::string credential) { return State::runOwned(state_, std::move(credential)); }
void DesktopSession::stop() { state_->stop(); }
} // namespace ultraterimal::agent
