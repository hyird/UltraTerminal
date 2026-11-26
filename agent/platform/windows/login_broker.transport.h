#pragma once

#include <asio/awaitable.hpp>
#include <blocking_pool.h>
#include <cstdint>
#include <event_loop.h>
#include <filesystem>
#include <memory>

namespace ultraterimal::agent {
// Own one login attempt for an already paired writable desktop connection.
// Creation outside the installed SYSTEM service returns nullptr. stop() and
// run() belong to the session's event loop; join run() before destroying its pool.
class WindowsLoginBroker {
  public:
    static std::unique_ptr<WindowsLoginBroker> create(ruvia::event_loop loop,
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
