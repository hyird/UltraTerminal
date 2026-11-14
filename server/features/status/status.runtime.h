#pragma once
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait.h>

namespace ultraterimal::server {
// Worker-owned invalidations. Only identifiers are queued; the writer rereads
// authorized state before delivery, so stale queued data cannot grant access.
class StatusSubscription {
  public:
    StatusSubscription(ruvia::worker_ref worker, std::string user);
    const std::string userId;
    bool enqueue(std::string event);
    ruvia::task<std::optional<std::string>> receive(ruvia::stop_token stop);
    void close();
    bool closed() const { return closed_; }

  private:
    std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>> queue_;
    std::set<std::string> pending_;
    bool closed_ = false;
};
class StatusHub {
  public:
    std::shared_ptr<StatusSubscription> subscribe(ruvia::worker_ref worker, const std::string& user);
    void publish(const std::string& event);
    void invalidate();

  private:
    std::vector<std::weak_ptr<StatusSubscription>> subscribers_;
};
} // namespace ultraterimal::server
