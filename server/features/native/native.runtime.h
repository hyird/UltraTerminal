#pragma once

#include <memory>

#include <ruvia/web/middleware.h>

#include "config/server.config.h"

namespace ultraterimal::server {
class NativeServer {
  public:
    explicit NativeServer(const ServerConfig& config);
    ~NativeServer();
    NativeServer(const NativeServer&) = delete;
    NativeServer& operator=(const NativeServer&) = delete;
    void start(ruvia::web_worker_ref businessWorker);
    void stop();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::server
