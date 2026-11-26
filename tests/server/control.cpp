#include <chrono>
#include <event_loop.h>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <task_scope.h>
#include <utility>

#include "features/control/control.runtime.h"

namespace {
ruvia::task<void> replyLogin(ultraterimal::server::ControlConnection& connection, bool disconnect) {
    auto bytes = co_await connection.receive({});
    if (!bytes) {
        throw std::runtime_error("Missing Windows credential request");
    }
    const auto request = ultraterimal::protocol::decodeMessage(*bytes);
    if (!request || !request->has_windows_login_request()) {
        throw std::runtime_error("Wrong Windows credential request");
    }
    if (disconnect) {
        connection.close();
    } else {
        ultraterimal::wire::WindowsLoginResult result;
        result.set_request_id(request->windows_login_request().request_id());
        result.set_success(true);
        if (!connection.windowsLoginReply(result)) {
            throw std::runtime_error("Valid Windows credential reply rejected");
        }
    }
}
ruvia::task<void> replyRestartLate(ultraterimal::server::ControlConnection& connection, ruvia::worker_ref worker, bool& accepted) {
    auto bytes = co_await connection.receive({});
    if (!bytes) {
        throw std::runtime_error("Missing restart request for timeout test");
    }
    const auto request = ultraterimal::protocol::decodeMessage(*bytes);
    if (!request || !request->has_windows_restart_request()) {
        throw std::runtime_error("Wrong restart request for timeout test");
    }
    (void)co_await ruvia::sleep_for(worker, std::chrono::seconds(6));
    ultraterimal::wire::WindowsRestartResult late;
    late.set_operation_id(request->windows_restart_request().operation_id());
    late.set_status(ultraterimal::wire::WindowsRestartResult::ACCEPTED);
    accepted = connection.windowsRestartReply(late);
    late.set_operation_id("not-a-sent-operation");
    if (connection.windowsRestartReply(late)) {
        throw std::runtime_error("Unknown restart operation was accepted");
    }
}
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Control queue invariant failed");
    }
}
ruvia::task<void> exercise(ruvia::worker_ref worker) {
    using namespace ultraterimal;
    server::ControlConnection connection(worker);
    wire::Envelope heartbeat;
    heartbeat.mutable_heartbeat();
    require(!connection.enqueue(heartbeat));
    connection.ready = true;
    for (int i = 0; i < 32; ++i) {
        require(connection.enqueue(heartbeat));
    }
    require(!connection.enqueue(heartbeat));
    auto bytes = co_await connection.receive({});
    require(bytes.has_value());
    const auto first = protocol::decodeMessage(*bytes);
    require(first && first->sequence() == 2);
    require(connection.enqueue(heartbeat));
    for (std::uint64_t sequence = 3; sequence <= 34; ++sequence) {
        bytes = co_await connection.receive({});
        const auto message = bytes ? protocol::decodeMessage(*bytes) : std::unexpected(protocol::MessageError::kInvalidField);
        require(message && message->sequence() == sequence);
    }
    wire::Envelope open;
    open.set_session_id("session");
    auto* session = open.mutable_session_open();
    session->set_kind(wire::DESKTOP);
    session->set_user_id("user");
    for (auto purpose : {wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO}) {
        auto* grant = session->add_channels();
        grant->set_channel_id(purpose == wire::DESKTOP_INPUT ? "input" : "video");
        grant->set_purpose(purpose);
        grant->set_ticket(std::string(4096, 't'));
    }
    int accepted = 0;
    while (connection.enqueue(open)) {
        require(++accepted <= 32);
    }
    require(accepted > 0 && accepted < 32); // Byte bound must reject before the count bound.
    for (int i = 0; i < accepted; ++i) {
        require((co_await connection.receive({})).has_value());
    }
    ruvia::stop_source stop;
    wire::WindowsLoginRequest login;
    login.set_action(wire::WindowsLoginRequest::READ);
    wire::WindowsRestartRequest restart;
    restart.set_operation_id(std::string(64, 'a'));
    restart.set_device_id("d-" + std::string(64, 'b'));
    restart.set_expires_at_unix_seconds(1);
    require(!connection.enqueueRestart(restart)); // Old Agent capability: definitely not queued.
    connection.capabilities.set_system_restart(true);
    require(connection.enqueueRestart(restart));
    auto restartBytes = co_await connection.receive({});
    require(restartBytes.has_value());
    const auto restartMessage = protocol::decodeMessage(*restartBytes);
    require(restartMessage && restartMessage->has_windows_restart_request());
    wire::WindowsRestartResult restartResult;
    restartResult.set_operation_id(restart.operation_id());
    restartResult.set_status(wire::WindowsRestartResult::ACCEPTED);
    require(connection.windowsRestartReply(restartResult));
    require(connection.windowsRestartReply(restartResult)); // Repeated known response remains correlated.
    wire::WindowsRestartResult unknownRestart;
    unknownRestart.set_operation_id("not-a-sent-operation");
    unknownRestart.set_status(wire::WindowsRestartResult::ACCEPTED);
    require(!connection.windowsRestartReply(unknownRestart));
    restart.set_operation_id(std::string(64, 'c'));
    bool lateAccepted = false;
    {
        const auto lateRepliesWorker = worker;
        ruvia::task_scope lateReplies(lateRepliesWorker);
        bool lateRepliesJoined = false;
        std::exception_ptr lateRepliesFailure;
        try {
            // enqueue is synchronous; an arbitrarily late ACK remains independently routable.
            require(connection.enqueueRestart(restart));
            lateReplies.spawn(replyRestartLate(connection, worker, lateAccepted));
            lateRepliesJoined = true;
            co_await lateReplies.join();
            require(lateAccepted);
            for (int i = 0; i < 32; ++i) {
                require(connection.enqueue(heartbeat));
            }
            restart.set_operation_id(std::string(64, 'd'));
            require(!connection.enqueueRestart(restart)); // A full queue is a definite local rejection.
            unknownRestart.set_operation_id(restart.operation_id());
            require(!connection.windowsRestartReply(unknownRestart));
            for (int i = 0; i < 32; ++i) {
                require((co_await connection.receive({})).has_value());
            }
            require(!(co_await connection.windowsLogin(login))); // Old Agent capability.
            connection.capabilities.set_windows_login_settings(true);
            require(!(co_await connection.windowsLogin(login))); // No reply: bounded timeout.
            auto expired = protocol::decodeMessage(*(co_await connection.receive({})));
            require(expired && expired->has_windows_login_request());
            wire::WindowsLoginResult late;
            late.set_request_id(expired->windows_login_request().request_id());
            require(connection.windowsLoginReply(late));
            late.set_request_id(late.request_id() + 1);
            require(!connection.windowsLoginReply(late)); // Unsolicited future correlation.
            {
                const auto repliesWorker = worker;
                ruvia::task_scope replies(repliesWorker);
                bool repliesJoined = false;
                std::exception_ptr repliesFailure;
                try {
                    replies.spawn(replyLogin(connection, false));
                    auto settings = co_await connection.windowsLogin(login);
                    require(settings && settings->success());
                    repliesJoined = true;
                    co_await replies.join();
                    {
                        const auto disconnectReplyWorker = worker;
                        ruvia::task_scope disconnectReply(disconnectReplyWorker);
                        bool disconnectReplyJoined = false;
                        std::exception_ptr disconnectReplyFailure;
                        try {
                            disconnectReply.spawn(replyLogin(connection, true));
                            settings = co_await connection.windowsLogin(login);
                            require(settings && !settings->success()); // Disconnect wakes pending request.
                            disconnectReplyJoined = true;
                            co_await disconnectReply.join();
                            stop.request_stop();
                            require(!(co_await connection.receive(stop.token())));
                            connection.close();
                            require(!connection.enqueue(heartbeat));
                            require(!(co_await connection.receive({})));

                        } catch (...) {
                            disconnectReplyFailure = std::current_exception();
                        }
                        if (!disconnectReplyJoined) {
                            disconnectReply.request_stop();
                            try {
                                co_await disconnectReply.join();
                            } catch (...) {
                                if (!disconnectReplyFailure) {
                                    disconnectReplyFailure = std::current_exception();
                                }
                            }
                        }
                        if (disconnectReplyFailure) {
                            std::rethrow_exception(disconnectReplyFailure);
                        }
                    }

                } catch (...) {
                    repliesFailure = std::current_exception();
                }
                if (!repliesJoined) {
                    replies.request_stop();
                    try {
                        co_await replies.join();
                    } catch (...) {
                        if (!repliesFailure) {
                            repliesFailure = std::current_exception();
                        }
                    }
                }
                if (repliesFailure) {
                    std::rethrow_exception(repliesFailure);
                }
            }

        } catch (...) {
            lateRepliesFailure = std::current_exception();
        }
        if (!lateRepliesJoined) {
            lateReplies.request_stop();
            try {
                co_await lateReplies.join();
            } catch (...) {
                if (!lateRepliesFailure) {
                    lateRepliesFailure = std::current_exception();
                }
            }
        }
        if (lateRepliesFailure) {
            std::rethrow_exception(lateRepliesFailure);
        }
    }
}
} // namespace
int main() {
    try {
        ruvia::event_loop_pool pool({.loop_count_ = 1});
        pool.start();
        const auto loop = pool.loop(0);
        auto task = loop.submit(exercise(loop.worker())).take_value();
        std::move(task).take_value();
        pool.join();
        std::cout << "Control readiness, queue count/bytes, FIFO sequencing, cancellation and close passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
