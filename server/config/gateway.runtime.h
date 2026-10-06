#pragma once
#include <atomic>
#include <memory>
#include <stop_token>
#include <string>

#include <ruvia/core/blocking_pool.h>

#include "modules/database/database.config.h"
#include "modules/database/database.service.h"

namespace ultraterimal::server {
class GatewayRuntime {
  public:
    explicit GatewayRuntime(const DatabaseConfig& database);
    ~GatewayRuntime();
    void start();
    void stop();
    bool failed() const;

  private:
    std::shared_ptr<DatabaseConnection> lease_;
    std::shared_ptr<std::atomic<bool>> failed_ = std::make_shared<std::atomic<bool>>(false);
    std::stop_source stop_;
    ruvia::blocking_pool pool_{{.thread_count_ = 1, .queue_capacity_ = 1}};
    bool started_ = false;
};
} // namespace ultraterimal::server
