#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <memory>

namespace ultraterimal::agent {
class ConsoleSessionSubscription;
// SCM owns the source. Notifications coalesce; consumers always query the
// current console rather than trusting the session named in an old event.
class ConsoleSessionEvents {
  public:
    ConsoleSessionEvents();
    ~ConsoleSessionEvents();
    ConsoleSessionEvents(const ConsoleSessionEvents&) = delete;
    ConsoleSessionEvents& operator=(const ConsoleSessionEvents&) = delete;
    void notify() noexcept;

  private:
    friend class ConsoleSessionSubscription;
    struct State;
    std::unique_ptr<State> state_;
};
class ConsoleSessionSubscription {
  public:
    ConsoleSessionSubscription(asio::any_io_executor executor, ConsoleSessionEvents& events);
    ~ConsoleSessionSubscription();
    ConsoleSessionSubscription(const ConsoleSessionSubscription&) = delete;
    ConsoleSessionSubscription& operator=(const ConsoleSessionSubscription&) = delete;
    asio::awaitable<void> wait();
    void cancel();

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ultraterimal::agent
