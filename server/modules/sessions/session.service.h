#pragma once
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "modules/devices/device.service.h"
#include "session.types.h"

namespace ultraterimal::server {
class SessionService {
  public:
    explicit SessionService(std::string database) : database_(std::move(database)) {}
    std::expected<IssuedSession, SessionFailure> create(const SessionRequest& request) const;
    std::optional<ChannelAuthorization> consumeBrowser(std::string_view userId, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const;
    std::optional<ChannelAuthorization> consumeAgent(const DeviceIdentity& device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const;
    bool close(std::string_view userId, std::string_view sessionId, SessionEnd reason) const;
    // Re-checks grants for an established channel; callers must end its live I/O on false.
    bool authorized(std::string_view sessionId) const;
    std::optional<SessionInfo> get(std::string_view userId, std::string_view sessionId) const;
    bool closeForDevice(const DeviceIdentity& device, std::string_view sessionId, SessionEnd reason) const;
    void disconnectDevice(const DeviceIdentity& device) const;
    std::vector<std::string> revokeDevice(std::string_view deviceId) const;
    // Startup only, while holding the single-gateway database lease.
    void recoverAfterRestart() const;

  private:
    std::optional<ChannelAuthorization> consume(std::string_view userId, const DeviceIdentity* device, std::string_view sessionId, std::string_view channelId, wire::ChannelPurpose purpose, std::string_view ticket) const;
    std::string database_;
};
} // namespace ultraterimal::server
