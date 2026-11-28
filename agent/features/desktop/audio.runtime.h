#pragma once

#include <asio/awaitable.hpp>
#include <event_loop.h>
#include <memory>
#include <optional>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {

struct DesktopAudioUpdate {
    std::optional<wire::AudioConfig> config;
    std::optional<wire::AudioFrame> frame;
};

// Runs WASAPI loopback capture + Opus encoding on a dedicated thread and emits
// AudioConfig / AudioFrame updates to the owning desktop worker event loop.
class DesktopAudioRuntime {
  public:
    explicit DesktopAudioRuntime(ruvia::event_loop loop);
    ~DesktopAudioRuntime();
    DesktopAudioRuntime(const DesktopAudioRuntime&) = delete;
    DesktopAudioRuntime& operator=(const DesktopAudioRuntime&) = delete;

    void setEnabled(bool enabled);
    void stop();
    asio::awaitable<std::optional<DesktopAudioUpdate>> next();

  private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ultraterimal::agent
