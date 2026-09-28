#pragma once

#include <asio/awaitable.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <ruvia/core/BlockingPool.h>
#include <ruvia/core/EventLoop.h>

namespace ultraterimal::agent {
// Own one login attempt for an already paired writable desktop connection.
// Creation outside the installed SYSTEM service returns nullptr. stop() and
// run() belong to the session's event loop; join run() before destroying its pool.
class WindowsLoginBroker {
  public:
    static std::unique_ptr<WindowsLoginBroker> create(ruvia::EventLoop loop, ruvia::BlockingPool& pool,
                                                      std::filesystem::path credentialFile, std::uint32_t session) noexcept;
    ~WindowsLoginBroker();
    asio::awaitable<void> run();
    void stop();

  private:
    struct State;
    explicit WindowsLoginBroker(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};
} // namespace ultraterimal::agent
