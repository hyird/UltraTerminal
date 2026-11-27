#pragma once
#include <atomic>
#include <db_executor.h>
#include <event_loop.h>
#include <memory>
#include <optional>
#include <stop_token.h>
#include <string>

#include "config/database.config.h"

namespace ultraterimal::server {
class GatewayRuntime {
  public:
    explicit GatewayRuntime(const DatabaseConfig& database);
    ~GatewayRuntime();
    void start();
    void stop();
    bool failed() const;

  private:
    std::string ownerId_;
    ruvia::event_loop_pool pool_{{.loop_count_ = 1, .queue_capacity_ = 64}};
    std::shared_ptr<ruvia::db_client> client_;
    std::shared_ptr<std::atomic<bool>> failed_ = std::make_shared<std::atomic<bool>>(false);
    ruvia::stop_source stop_;
    std::optional<ruvia::root_task<void>> watcher_;
    bool started_ = false;
    bool stopped_ = false;
};
} // namespace ultraterimal::server
