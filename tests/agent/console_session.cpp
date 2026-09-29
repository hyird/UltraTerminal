#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "platform/windows/console_session.transport.h"

namespace {
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
} // namespace
int main() {
    using namespace ultraterimal::agent;
    try {
        asio::io_context io;
        ConsoleSessionEvents events;
        ConsoleSessionSubscription first(io.get_executor(), events), second(io.get_executor(), events);
        unsigned delivered = 0;
        std::exception_ptr failure;
        const auto done = [&](std::exception_ptr error) {
            failure = error ? error : failure;
            ++delivered;
        };
        // Signals arriving before an async wait remain pending. A burst is
        // coalesced independently for every subscriber, without callback queues.
        for (unsigned n = 0; n < 100; ++n) {
            events.notify();
        }
        asio::co_spawn(io, first.wait(), done);
        asio::co_spawn(io, second.wait(), done);
        io.run();
        require(!failure && delivered == 2, "SCM notifications did not fan out");
        io.restart();
        bool cancelled = false;
        asio::co_spawn(io, first.wait(), [&](std::exception_ptr error) {
            try {
                if (error) {
                    std::rethrow_exception(error);
                }
            } catch (const std::system_error& exception) {
                cancelled = exception.code() == asio::error::operation_aborted;
            }
        });
        asio::post(io, [&] { first.cancel(); });
        io.run();
        require(cancelled, "Burst leaked extra notifications or cancellation was lost");
        {
            std::vector<std::unique_ptr<ConsoleSessionSubscription>> subscriptions;
            for (unsigned n = 0; n < 14; ++n) {
                subscriptions.push_back(std::make_unique<ConsoleSessionSubscription>(io.get_executor(), events));
            }
            bool rejected = false;
            try {
                ConsoleSessionSubscription excess(io.get_executor(), events);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            require(rejected, "Subscription limit was not bounded");
        }
        ConsoleSessionSubscription replacement(io.get_executor(), events);
        events.notify();
        io.restart();
        asio::co_spawn(io, replacement.wait(), done);
        io.run();
        require(!failure && delivered == 3, "Expired subscriptions were not reclaimed");
        std::cout << "Console notification coalescing, bounds and cancellation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
