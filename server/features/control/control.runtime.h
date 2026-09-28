#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <ruvia/core/Channel.h>
#include <ruvia/core/OneShot.h>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
// All access belongs to the gateway Worker, including sequence allocation and queue accounting.
class ControlConnection {
  public:
    explicit ControlConnection(ruvia::WorkerHandle worker);
    bool ready = false;
    wire::Capabilities capabilities;
    bool enqueue(wire::Envelope message);
    ruvia::Task<std::optional<std::string>> receive(ruvia::StopToken stop);
    void close();
    ruvia::Task<std::optional<wire::WindowsLoginResult>> windowsLogin(wire::WindowsLoginRequest request);
    bool windowsLoginReply(const wire::WindowsLoginResult& result);
    bool enqueueRestart(wire::WindowsRestartRequest request);
    bool windowsRestartReply(const wire::WindowsRestartResult& result);
    std::vector<std::string> restartOperations() const;

  private:
    ruvia::WorkerHandle worker_;
    std::optional<ruvia::OneShotCompletion<wire::WindowsLoginResult>> loginPending_;
    std::uint64_t loginRequest_ = 0;
    std::deque<std::string> sentRestartOperations_;
    std::pair<ruvia::ChannelSender<std::string>, ruvia::ChannelReceiver<std::string>> queue_;
    std::uint64_t sequence_ = 1; // WELCOME uses sequence 1 before the writer starts sending.
    std::size_t queuedBytes_ = 0;
};
} // namespace ultraterimal::server
