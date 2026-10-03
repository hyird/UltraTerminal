#pragma once
#include <array>
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

  private:
    enum class Mode { kNone,
                      kVideo,
                      kLossless };
    bool acceptFrame(const wire::Envelope& message);
    bool acceptPatches(const google::protobuf::RepeatedPtrField<wire::PatchRect>& patches);
    DesktopChannelBinding binding_;
    std::uint64_t sequence_ = 1;
    std::uint64_t generation_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t timestamp_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Mode mode_ = Mode::kNone;
    bool refined_ = false;
    std::array<std::pair<std::uint32_t, std::uint32_t>, kDesktopTileCacheSlots> tiles_{};
};
} // namespace ultraterimal::protocol
