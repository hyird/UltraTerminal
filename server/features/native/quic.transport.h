#pragma once

#include <functional>
#include <memory>

#include <ruvia/core/EventLoop.h>

#include "config/server.config.h"
#include "stream.transport.h"

namespace ultraterimal::server {
class QuicListener {
  public:
    QuicListener(ruvia::EventLoop loop, const ServerConfig& config, std::function<void(std::unique_ptr<NativeStream>)> accept);
    ~QuicListener();
    QuicListener(const QuicListener&) = delete;
    QuicListener& operator=(const QuicListener&) = delete;
    asio::awaitable<void> run();
    void stop();

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::server
