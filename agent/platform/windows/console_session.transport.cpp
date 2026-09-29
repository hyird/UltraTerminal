#include "console_session.transport.h"

#include <algorithm>
#include <asio/use_awaitable.hpp>
#include <asio/windows/object_handle.hpp>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <Windows.h>

namespace ultraterimal::agent {
namespace {
struct Notification {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Notification() {
        if (!event) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Create console notification event");
        }
    }
    ~Notification() { CloseHandle(event); }
};
} // namespace
struct ConsoleSessionEvents::State {
    std::mutex mutex;
    std::vector<std::weak_ptr<Notification>> subscriptions;
};
struct ConsoleSessionSubscription::State {
    std::shared_ptr<Notification> notification = std::make_shared<Notification>();
    asio::windows::object_handle event;
    explicit State(asio::any_io_executor executor) : event(executor) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), notification->event, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Duplicate console notification event");
        }
        std::error_code error;
        event.assign(duplicate, error);
        if (error) {
            CloseHandle(duplicate);
            throw std::system_error(error);
        }
    }
};
ConsoleSessionEvents::ConsoleSessionEvents() : state_(std::make_unique<State>()) {}
ConsoleSessionEvents::~ConsoleSessionEvents() = default;
void ConsoleSessionEvents::notify() noexcept {
    try {
        std::lock_guard lock(state_->mutex);
        for (const auto& subscription : state_->subscriptions) {
            if (auto notification = subscription.lock()) {
                SetEvent(notification->event);
            }
        }
    } catch (...) {
        // Never propagate an exception into the SCM C callback.
    }
}
ConsoleSessionSubscription::ConsoleSessionSubscription(asio::any_io_executor executor, ConsoleSessionEvents& events)
    : state_(std::make_unique<State>(std::move(executor))) {
    std::lock_guard lock(events.state_->mutex);
    auto& subscriptions = events.state_->subscriptions;
    std::erase_if(subscriptions, [](const auto& entry) { return entry.expired(); });
    if (subscriptions.size() >= 16) {
        throw std::runtime_error("Console subscription limit exceeded");
    }
    subscriptions.push_back(state_->notification);
}
ConsoleSessionSubscription::~ConsoleSessionSubscription() = default;
asio::awaitable<void> ConsoleSessionSubscription::wait() { co_await state_->event.async_wait(asio::use_awaitable); }
void ConsoleSessionSubscription::cancel() { state_->event.cancel(); }
} // namespace ultraterimal::agent
