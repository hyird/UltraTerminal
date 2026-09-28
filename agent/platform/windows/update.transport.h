#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "ultraterimal/protocol/update.protocol.h"

namespace ultraterimal::agent {
bool launchAgentUpdate(const std::filesystem::path& executable, const wire::AgentUpdate& update);
class WindowsAgentUpdate {
  public:
    WindowsAgentUpdate();
    ~WindowsAgentUpdate();
    std::filesystem::path root() const;
    const wire::AgentUpdate& request() const;
    int install();
    void failed(std::uint32_t code) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};
std::optional<std::uint32_t> takeAgentUpdateResult(const std::filesystem::path& root);
} // namespace ultraterimal::agent
