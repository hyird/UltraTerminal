#include "status.runtime.h"

#include <algorithm>
#include <utility>

namespace ultraterimal::server {
StatusSubscription::StatusSubscription(ruvia::worker_ref worker, std::string user)
    : userId(std::move(user)), queue_(ruvia::make_channel<std::string>(std::move(worker), {.capacity_ = 64})) {}
bool StatusSubscription::enqueue(std::string event) {
    if (closed_ || event.empty() || event.size() > 130) {
        return false;
    }
    if (pending_.contains(event)) {
        return true;
    }
    if (pending_.size() >= 64) {
        close();
        return false;
    }
    pending_.insert(event);
    if (!queue_.first.send(std::move(event)).accepted()) {
        close();
        return false;
    }
    return true;
}
ruvia::task<std::optional<std::string>> StatusSubscription::receive(ruvia::stop_token stop) {
    auto result = co_await queue_.second.receive(std::move(stop));
    if (!result.has_value() || closed_) {
        co_return std::nullopt;
    }
    auto event = std::move(result).take_value();
    pending_.erase(event);
    co_return event;
}
void StatusSubscription::close() {
    closed_ = true;
    queue_.first.close();
    pending_.clear();
}
std::shared_ptr<StatusSubscription> StatusHub::subscribe(ruvia::worker_ref worker, const std::string& user) {
    std::erase_if(subscribers_, [](const auto& weak) { const auto entry = weak.lock(); return !entry || entry->closed(); });
    if (subscribers_.size() >= 128 || std::count_if(subscribers_.begin(), subscribers_.end(), [&](const auto& weak) { const auto entry = weak.lock(); return entry && entry->userId == user; }) >= 4) {
        return {};
    }
    auto subscription = std::make_shared<StatusSubscription>(std::move(worker), user);
    subscribers_.push_back(subscription);
    return subscription;
}
void StatusHub::publish(const std::string& event) {
    if (event.size() < 3 || event.size() > 130 || (event[0] != 'd' && event[0] != 'u') || event[1] != ':') {
        return;
    }
    for (const auto& weak : subscribers_) {
        if (const auto entry = weak.lock(); entry && (event[0] == 'd' || event.substr(2) == "*" || event.substr(2) == entry->userId)) {
            entry->enqueue(event);
        }
    }
}
void StatusHub::invalidate() {
    for (const auto& weak : subscribers_) {
        if (const auto entry = weak.lock()) {
            entry->close();
        }
    }
}
} // namespace ultraterimal::server
