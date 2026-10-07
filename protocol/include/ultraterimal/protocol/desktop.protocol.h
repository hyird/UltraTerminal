#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kDesktopTileCacheSlots = 128;
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
    // Input ACKs are format/direction checked, not matched to the latest frame.
    void setOptimizationContract(std::uint32_t contract) noexcept;
    void setContinuousFrames(bool allowed) noexcept;

  private:
    enum class Mode { kNone,
                      kVideo,
                      kLossless };
    bool acceptFrame(const wire::Envelope& message);
    bool acceptPatches(const google::protobuf::RepeatedPtrField<wire::PatchRect>& patches);
    bool acceptRefinement(const wire::VideoRefinement& refinement);
    void resetRefinement() noexcept;
    DesktopChannelBinding binding_;
    std::uint64_t sequence_ = 1;
    std::uint64_t generation_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t contentFrame_ = 0;
    std::uint64_t timestamp_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Mode mode_ = Mode::kNone;
    bool refined_ = false;
    std::uint32_t allowedOptimizationContract_ = 0;
    std::uint32_t activeOptimizationContract_ = 0;
    bool allowedContinuousFrames_ = false;
    bool activeContinuousFrames_ = false;
    std::uint64_t refinementBatch_ = 0;
    std::size_t refinementBytes_ = 0;
    std::size_t refinementTiles_ = 0;
    // Maximum 16384x16384 canvas on the canonical 128x64 grid.
    std::bitset<128 * 256> refinementGrid_;
    std::array<std::pair<std::uint32_t, std::uint32_t>, kDesktopTileCacheSlots> tiles_{};
};
} // namespace ultraterimal::protocol
