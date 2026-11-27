#pragma once
#include <cstdint>
#include <expected>
#include <string>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
enum class ChannelSource { kBrowser,
                           kAgent };
enum class DesktopMessageError { kMalformed,
                                 kBinding,
                                 kDirection,
                                 kReadOnly,
                                 kReplay,
                                 kFrameState };
struct DesktopChannelBinding {
    std::string sessionId;
    std::string channelId;
    wire::ChannelPurpose purpose = wire::CHANNEL_UNSPECIFIED;
    ChannelSource source = ChannelSource::kBrowser;
    bool writeControl = false;
    std::uint32_t optimizationContract = 0;
};
// One validator per sending side, after CHANNEL_OPEN (sequence 1) has been authorized.
// Failed messages never advance its sequence or image baseline.
class DesktopChannelValidator {
  public:
    explicit DesktopChannelValidator(DesktopChannelBinding binding);
    DesktopChannelValidator(const DesktopChannelValidator&) = delete;
    DesktopChannelValidator& operator=(const DesktopChannelValidator&) = delete;
    DesktopChannelValidator(DesktopChannelValidator&&) noexcept = default;
    DesktopChannelValidator& operator=(DesktopChannelValidator&&) noexcept = default;
    std::expected<void, DesktopMessageError> accept(const wire::Envelope& message);
    // Exact version 2 permits v2 at the NEXT VideoConfig/generation barrier.
    // Zero/unknown disables permission, without changing the active generation.
    void setOptimizationContract(std::uint32_t contract) noexcept;
    void setContinuousFrames(bool allowed) noexcept;

  private:
    bool acceptFrame(const wire::Envelope& message);
    DesktopChannelBinding binding_;
    std::uint64_t sequence_ = 1;
    std::uint64_t generation_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t contentFrame_ = 0;
    std::uint64_t timestamp_ = 0;
    std::uint32_t allowedOptimizationContract_ = 0;
    std::uint32_t activeOptimizationContract_ = 0;
    bool allowedContinuousFrames_ = false;
    bool activeContinuousFrames_ = false;
};
} // namespace ultraterimal::protocol
