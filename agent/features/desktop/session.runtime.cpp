#include "session.runtime.h"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/BlockingPool.h>

#include "bridge.runtime.h"
#include "desktop.transport.h"
#include "platform/windows/desktop_failure.error.h"
#include "platform/windows/desktop_launch.transport.h"
#include "platform/windows/desktop_process.transport.h"
#include "platform/windows/login_broker.transport.h"
#include "platform/windows/pipe.transport.h"

namespace ultraterimal::agent {
namespace {
struct Resources {
    std::unique_ptr<NamedPipeTransport> pipe;
    std::unique_ptr<DesktopProcess> child;
};
} // namespace
struct DesktopSession::State : std::enable_shared_from_this<State> {
    ruvia::EventLoop loop;
    // Launch finishes before the bridge and login broker start. Each then awaits
    // at most one job; allow both submissions before the worker wakes up.
    ruvia::BlockingPool systemPool{{.threadCount = 1, .queueCapacity = 2}};
    DesktopTransport remote;
    asio::steady_timer grace;
    std::filesystem::path executable;
    std::filesystem::path loginCredential;
    std::unique_ptr<WindowsLoginBroker> login;
    std::string session;
    std::uint32_t windowsSession;
    bool writeControl;
    std::uint32_t desktopInputContract;
    bool started = false, stopping = false, joined = false;
    Resources resources;
    DesktopSessionResult result;

    State(ruvia::EventLoop owner, ConnectionConfig config, wire::Envelope grant,
          std::filesystem::path path, std::uint32_t selected)
        : loop(owner), remote(owner, config, grant), grace(owner.executor()),
          executable(std::move(path)), loginCredential(config.credentialFile.parent_path() / "windows_login.credential"), session(grant.session_id()), windowsSession(selected),
          writeControl(grant.session_open().write_control()), desktopInputContract(grant.session_open().desktop_input_contract()) {
        if ((writeControl && desktopInputContract != 2) || (!writeControl && desktopInputContract != 0)) {
            throw std::invalid_argument("Desktop input contract is unsupported");
        }
        if (!selected || selected == 0xffffffff) {
            throw std::invalid_argument("Invalid Windows desktop session");
        }
        // The transport owns the tickets; do not retain a second grant copy.
    }
    ~State() { systemPool.join(); }
    void cancelIo() {
        if (login) {
            login->stop();
        }
        remote.cancel();
        if (resources.pipe) {
            resources.pipe->cancel();
        }
    }
    void stop() {
        stopping = true;
        cancelIo();
    }
    void terminalError(const char* code, const char* message) {
        result.terminal.Clear();
        result.terminal.mutable_session_error()->set_code(code);
        result.terminal.mutable_session_error()->set_message(message);
    }
    asio::awaitable<void> bridge() {
        try {
            result.terminal = co_await bridgeDesktop(loop, systemPool, remote, *resources.pipe, session, windowsSession, 0, writeControl, desktopInputContract);
        } catch (...) {
            if (login) {
                login->stop();
            }
            throw;
        }
        if (login) {
            login->stop();
        }
    }
    static asio::awaitable<DesktopSessionResult> runOwned(std::shared_ptr<State> self, std::string credential) {
        if (self->started) {
            throw std::logic_error("Desktop session can only run once");
        }
        self->started = true;
        // Explicit stop owns cancellation. Never abandon a launch job or child
        // wait because a parent coroutine cancelled its await expression.
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        const char* stage = "native_pairing";
        try {
            if (self->stopping) {
                throw std::runtime_error("Desktop session stopped before pairing");
            }
            co_await self->remote.connect(std::move(credential));
            stage = "worker_launch";
            auto launch = ruvia::runBlocking(self->systemPool, self->loop.handle(),
                                             [executor = self->loop.executor(), selected = self->windowsSession, executable = self->executable] {
                                                 DesktopIdentity user(selected);
                                                 Resources resources;
                                                 resources.pipe = std::make_unique<NamedPipeTransport>(executor, user.pipeSid());
                                                 resources.child = std::make_unique<DesktopProcess>(executor, user, executable, resources.pipe->name());
                                                 return resources;
                                             });
            self->resources = co_await ruvia::asAwaitable(std::move(launch));
            self->result.processId = self->resources.child->id();
            if (self->stopping) {
                throw std::runtime_error("Desktop session stopped during launch");
            }
            stage = "worker_pipe_pairing";
            co_await self->resources.pipe->accept(self->result.processId, self->windowsSession);
            stage = "login_broker";
            if (self->writeControl) {
                self->login = WindowsLoginBroker::create(self->loop, self->systemPool, self->loginCredential, self->windowsSession);
            }
            stage = "session_bridge";
            if (self->login) {
                using namespace asio::experimental::awaitable_operators;
                co_await (self->bridge() && self->login->run());
            } else {
                co_await self->bridge();
            }
        } catch (const std::exception& error) {
            if (!self->stopping) {
                reportDesktopFailure(stage, error.what(), self->session);
                auto point = desktopFailureStage(stage);
                if (const auto* fault = dynamic_cast<const DesktopFailure*>(&error); fault && fault->stage() != wire::DesktopFault::STAGE_UNKNOWN) {
                    point = fault->stage();
                }
                self->result.fault.emplace();
                self->result.fault->set_stage(point);
                self->result.fault->set_code(desktopFailureCode(std::current_exception(), point));
                self->result.fault->set_windows_session_id(self->windowsSession);
                self->result.fault->set_process_id(self->result.processId);
            }
            self->terminalError("desktop_failed", "Desktop session could not continue");
        }
        self->cancelIo();
        if (self->resources.child) {
            self->grace.expires_after(std::chrono::seconds(5));
            self->grace.async_wait([weak = self->weak_from_this()](std::error_code error) {
                if (auto state = weak.lock(); state && !error && !state->joined) {
                    state->result.forcedExit = true;
                    try {
                        state->resources.child->terminate();
                    } catch (...) {
                        // Closing the kill-on-close job remains the cleanup path.
                        state->resources.child.reset();
                    }
                }
            });
            const auto exitCode = co_await self->resources.child->wait();
            self->result.recordWorkerExit(exitCode, self->windowsSession);
            self->joined = true;
            self->grace.cancel();
            if (self->result.forcedExit) {
                self->terminalError("desktop_exit_timeout", "Desktop process exceeded its exit deadline");
            }
        }
        self->resources = {};
        if (self->stopping && !self->result.forcedExit) {
            self->result.terminal.Clear();
            self->result.terminal.mutable_session_close()->set_reason("cancelled");
            self->result.fault.reset();
        }
        self->result.terminal.set_protocol_version(protocol::kVersion);
        self->result.terminal.set_session_id(self->session);
        self->result.terminal.set_sequence(1); // Control writer assigns its stream sequence.
        co_return std::move(self->result);
    }
};
DesktopSession::DesktopSession(ruvia::EventLoop loop, ConnectionConfig config, wire::Envelope grant,
                               std::filesystem::path executable, std::uint32_t windowsSession)
    : state_(std::make_shared<State>(std::move(loop), std::move(config), std::move(grant), std::move(executable), windowsSession)) {}
DesktopSession::~DesktopSession() { stop(); }
asio::awaitable<DesktopSessionResult> DesktopSession::run(std::string credential) { return State::runOwned(state_, std::move(credential)); }
void DesktopSession::stop() { state_->stop(); }
} // namespace ultraterimal::agent
