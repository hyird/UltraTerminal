#include <asio/co_spawn.hpp>
#include <blocking_pool.h>
#include <event_loop.h>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

#include "features/desktop/tool.runtime.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireLogicError(const std::exception_ptr& failure, const char* message) {
    require(static_cast<bool>(failure), message);
    try {
        std::rethrow_exception(failure);
    } catch (const std::logic_error&) {
    } catch (...) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    using namespace ultraterimal;
    try {
        ruvia::blocking_pool blockingPool{{.thread_count_ = 2, .queue_capacity_ = 16}};
        ruvia::event_loop_pool loopPool{{.loop_count_ = 2, .queue_capacity_ = 64, .blocking_pool_ = &blockingPool}};
        auto controlLoop = loopPool.loop_for("control");
        ruvia::event_loop otherLoop;
        for (int i = 0; i < 8; ++i) {
            auto candidate = loopPool.next_loop();
            if (&candidate.io_context() != &controlLoop.io_context()) {
                otherLoop = candidate;
                break;
            }
        }
        require(otherLoop.valid(), "Failed to locate a secondary event loop");

        loopPool.start();
        const auto nonexistentExe = std::filesystem::temp_directory_path() / L"ultraterimal-nonexistent-desktop-tool.exe";

        {
            wire::DesktopActionRequest req;
            req.set_request_id(101);
            req.set_action(wire::DesktopActionRequest::ACTION_MOVE);
            req.set_x(10);
            req.set_y(20);
            req.set_normalized(true);

            std::promise<std::exception_ptr> done;
            auto future = done.get_future();
            asio::co_spawn(
                controlLoop.executor(),
                agent::runDesktopAction(otherLoop, nonexistentExe, req),
                [&done](std::exception_ptr failure, wire::DesktopActionResult) {
                    done.set_value(std::move(failure));
                });
            requireLogicError(future.get(), "Cross-loop runDesktopAction must reject mismatched worker");
        }

        {
            wire::DesktopSnapshotStart snapReq;
            snapReq.set_snapshot_id(std::string(64, 'a'));
            snapReq.set_max_width(640);
            snapReq.set_max_height(360);

            std::promise<std::exception_ptr> done;
            auto future = done.get_future();
            asio::co_spawn(
                controlLoop.executor(),
                agent::runDesktopSnapshot(otherLoop, agent::ConnectionConfig{}, nonexistentExe, snapReq),
                [&done](std::exception_ptr failure) {
                    done.set_value(std::move(failure));
                });
            requireLogicError(future.get(), "Cross-loop runDesktopSnapshot must reject mismatched worker");
        }

        for (std::uint64_t i = 1; i <= 4; ++i) {
            auto toolLoop = loopPool.next_loop();
            wire::DesktopActionRequest req;
            req.set_request_id(i);
            req.set_action(wire::DesktopActionRequest::ACTION_MOVE);
            req.set_x(100);
            req.set_y(100);
            req.set_normalized(true);

            std::promise<void> done;
            auto future = done.get_future();
            bool deliveredOnOwner = false;
            std::exception_ptr unexpectedFailure;
            wire::DesktopActionResult actionResult;

            asio::co_spawn(
                toolLoop.executor(),
                agent::runDesktopAction(toolLoop, nonexistentExe, req),
                [controlLoop, &done, &deliveredOnOwner, &unexpectedFailure, &actionResult](std::exception_ptr failure, wire::DesktopActionResult result) mutable {
                    auto deliver = [controlLoop, &done, &deliveredOnOwner, &unexpectedFailure, &actionResult, failure = std::move(failure), result = std::move(result)]() mutable {
                        deliveredOnOwner = controlLoop.worker().is_current();
                        unexpectedFailure = std::move(failure);
                        actionResult = std::move(result);
                        done.set_value();
                    };
                    if (controlLoop.worker().is_current()) {
                        deliver();
                    } else {
                        (void)controlLoop.defer_cleanup(std::move(deliver));
                    }
                });

            future.get();
            require(deliveredOnOwner, "Completion did not return to control loop");
            require(!unexpectedFailure, "Bound-loop runDesktopAction threw unexpected exception");
            require(actionResult.request_id() == i, "Desktop action response request_id mismatch");
            require(!actionResult.success() && actionResult.error() == wire::DESKTOP_TOOL_ERROR_UNAVAILABLE, "Unexpected desktop action error for missing executable");
        }

        loopPool.request_stop();
        loopPool.join();
        blockingPool.join();
        std::cout << "Desktop tool worker affinity and round-robin dispatch passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
