#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <ruvia/core/channel.h>
#include <ruvia/core/one_shot.h>
#include <ruvia/core/task.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
// All access belongs to the gateway Worker, including sequence allocation and queue accounting.
class ControlConnection {
  public:
    explicit ControlConnection(ruvia::worker_handle worker);
    bool ready = false;
    wire::Capabilities capabilities;
    std::string agentVersion;
    bool updatePending = false;
    bool enqueue(wire::Envelope message);
    ruvia::task<std::optional<std::string>> receive(ruvia::stop_token stop);
    void close();
    ruvia::task<std::optional<wire::WindowsLoginResult>> windowsLogin(wire::WindowsLoginRequest request);
    bool windowsLoginReply(const wire::WindowsLoginResult& result);
    bool enqueueRestart(wire::WindowsRestartRequest request);
    bool windowsRestartReply(const wire::WindowsRestartResult& result);
    std::vector<std::string> restartOperations() const;

  private:
    ruvia::worker_handle worker_;
    std::optional<ruvia::one_shot_completion<wire::WindowsLoginResult>> loginPending_;
    std::uint64_t loginRequest_ = 0;
    std::deque<std::string> sentRestartOperations_;
    std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>> queue_;
    std::uint64_t sequence_ = 1; // WELCOME uses sequence 1 before the writer starts sending.
    std::size_t queuedBytes_ = 0;
};
} // namespace ultraterimal::server
