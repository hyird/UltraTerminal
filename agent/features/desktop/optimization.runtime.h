#pragma once

#include <cstdint>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Agent-owned negotiation: a Config2 advertises support, NOT activation.
// Request changes only affect permission for the next generation barrier.
class DesktopOptimizationGate {
  public:
    explicit DesktopOptimizationGate(std::uint32_t serverContract = 0) : serverContract_(serverContract == 2 ? 2 : 0) {}
    static std::uint32_t serverContract(std::uint32_t input, std::uint32_t video) {
        return input == 2 && video == 2 ? 2 : 0;
    }
    std::uint32_t serverContract() const { return serverContract_; }
    void request(std::uint32_t browserContract) { requestedContract_ = browserContract == 2 ? 2 : 0; }
    std::uint32_t allowedContract() const { return serverContract_ == 2 && requestedContract_ == 2 ? 2 : 0; }
    // Call only after successful validation of a NEW VideoConfig/generation.
    void configure(const wire::VideoConfig& configuration) {
        activeContract_ = allowedContract() == 2 && configuration.desktop_optimization_contract() == 2 ? 2 : 0;
    }
    std::uint32_t activeContract() const { return activeContract_; }

  private:
    std::uint32_t serverContract_ = 0, requestedContract_ = 0, activeContract_ = 0;
};
} // namespace ultraterimal::agent
