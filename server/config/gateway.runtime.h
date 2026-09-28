#pragma once
#include <atomic>
#include <memory>
#include <stop_token>
#include <string>

#include <ruvia/core/BlockingPool.h>

#include "modules/database/database.service.h"

namespace ultraterimal::server {
class GatewayRuntime {
  public:
    explicit GatewayRuntime(const std::string& database);
    ~GatewayRuntime();
    void start();
    void stop();
    bool failed() const;

  private:
    std::shared_ptr<pqxx::connection> lease_;
    std::shared_ptr<std::atomic<bool>> failed_ = std::make_shared<std::atomic<bool>>(false);
    std::stop_source stop_;
    ruvia::BlockingPool pool_{{.threadCount = 1, .queueCapacity = 1}};
    bool started_ = false;
};
} // namespace ultraterimal::server
