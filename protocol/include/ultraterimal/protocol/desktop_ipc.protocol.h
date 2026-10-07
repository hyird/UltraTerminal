#pragma once
#include <cstdint>
#include <expected>
#include <string>

#include "desktop.protocol.h"

namespace ultraterimal::protocol {
enum class DesktopPipeSource { kService,
                               kDesktop };
struct DesktopPipeBinding {
    std::string sessionId;
    std::uint32_t windowsSessionId = 0;
    bool writeControl = false;
    DesktopPipeSource source = DesktopPipeSource::kService;
    std::uint32_t optimizationContract = 0;
};
// Each pipe direction has one sequence space. Local channel IDs are "input" and
// "video"; the service's native transport assigns the remote channel IDs/sequences.
class DesktopPipeValidator {
  public:
    explicit DesktopPipeValidator(DesktopPipeBinding binding);
    std::expected<void, DesktopMessageError> accept(const wire::Envelope& message);
    // Propagates permission to both child validators; no mid-generation switch.
    // The service passes the actual Server gate from DesktopStart explicitly.
    void setOptimizationContract(std::uint32_t contract) noexcept;
    void setContinuousFrames(bool allowed) noexcept;

  private:
    DesktopPipeBinding binding_;
    DesktopChannelValidator input_;
    DesktopChannelValidator video_;
    std::uint64_t sequence_ = 0;
    bool started_ = false;
    bool ended_ = false;
};
} // namespace ultraterimal::protocol
