#pragma once
#include <cstdint>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::uint32_t kDesktopSessionVersion = 1;
inline constexpr std::uint64_t kDesktopInputFreshness = 1;
inline constexpr std::uint64_t kDesktopAudio = 2;
struct DesktopSessionAgreement {
    std::uint32_t version = 0;
    std::uint64_t features = 0;
    bool operator==(const DesktopSessionAgreement&) const = default;
};
inline bool validDesktopSessionCapabilities(const wire::DesktopSessionCapabilities& offer) {
    return offer.minimum_version() > 0 && offer.minimum_version() <= offer.maximum_version() && offer.maximum_version() <= 65535;
}
inline wire::DesktopSessionCapabilities desktopSessionCapabilities(bool desktopAudio = false) {
    wire::DesktopSessionCapabilities offer;
    offer.set_minimum_version(kDesktopSessionVersion);
    offer.set_maximum_version(kDesktopSessionVersion);
    offer.set_features(kDesktopInputFreshness | (desktopAudio ? kDesktopAudio : 0));
    return offer;
}
inline DesktopSessionAgreement negotiateDesktopSession(const wire::DesktopSessionCapabilities& peer) {
    if (!validDesktopSessionCapabilities(peer) || peer.minimum_version() > kDesktopSessionVersion || peer.maximum_version() < kDesktopSessionVersion) {
        return {};
    }
    return {kDesktopSessionVersion, peer.features() & kDesktopInputFreshness};
}
} // namespace ultraterimal::protocol
