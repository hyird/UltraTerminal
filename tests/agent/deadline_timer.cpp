#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "platform/windows/deadline_timer.transport.h"

int main() {
    using namespace std::chrono_literals;
    using ultraterimal::agent::WindowsDeadlineTimer;
    try {
        asio::io_context loop;
        WindowsDeadlineTimer timer(loop.get_executor());
        unsigned completed = 0;
        const auto deadline = WindowsDeadlineTimer::Clock::now() + 5ms;
        timer.expiresAt(deadline);
        timer.asyncWait([&](std::error_code error) {
            if (error || WindowsDeadlineTimer::Clock::now() < deadline) {
                throw std::runtime_error("Deadline completed early or failed");
            }
            ++completed;
            timer.expiresAfter(1h);
            timer.asyncWait([&](std::error_code canceled) {
                if (canceled != asio::error::operation_aborted) {
                    throw std::runtime_error("Canceled deadline completed normally");
                }
                ++completed;
            });
            asio::post(loop, [&] { timer.cancel(); });
        });
        loop.run();
        if (completed != 2) {
            throw std::runtime_error("Deadline or cancellation completion was lost");
        }
        loop.restart();
        bool initiated = false;
        timer.expiresAt(WindowsDeadlineTimer::Clock::now() - 1ms);
        timer.asyncWait([&](std::error_code error) {
            if (error || !initiated) {
                throw std::runtime_error("Expired deadline failed or completed inline");
            }
            ++completed;
        });
        initiated = true;
        loop.run();
        return completed == 3 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
