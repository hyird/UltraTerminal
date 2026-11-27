#pragma once
#include <channel.h>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <one_shot.h>
#include <optional>
#include <string>
#include <task.h>
#include <unordered_map>
#include <vector>

#include "ultraterimal/protocol/file.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
// All access belongs to the gateway Worker, including sequence allocation and queue accounting.
class ControlConnection {
  public:
    explicit ControlConnection(ruvia::worker_ref worker);
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
    ruvia::task<std::optional<wire::FileListResult>> fileList(wire::FileListRequest request);
    bool fileListReply(const wire::FileListResult& result);
    ruvia::task<std::optional<wire::FileStatResult>> fileStat(wire::FileStatRequest request);
    bool fileStatReply(const wire::FileStatResult& result);
    bool enqueueFileTransfer(wire::FileTransferStart request);

  private:
    ruvia::worker_ref worker_;
    std::optional<ruvia::one_shot_completion<wire::WindowsLoginResult>> loginPending_;
    std::uint64_t loginRequest_ = 0;
    std::unordered_map<std::uint64_t, ruvia::one_shot_completion<wire::FileListResult>> fileListPending_;
    std::uint64_t fileListRequest_ = 0;
    std::unordered_map<std::uint64_t, ruvia::one_shot_completion<wire::FileStatResult>> fileStatPending_;
    std::uint64_t fileStatRequest_ = 0;
    std::deque<std::string> sentRestartOperations_;
    std::pair<ruvia::channel_sender<std::string>, ruvia::channel_receiver<std::string>> queue_;
    std::uint64_t sequence_ = 1; // WELCOME uses sequence 1 before the writer starts sending.
    std::size_t queuedBytes_ = 0;
};
} // namespace ultraterimal::server
