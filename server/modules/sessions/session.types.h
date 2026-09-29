#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::server {
enum class SessionFailure { kInvalid,
                            kDenied,
                            kApprovalRequired,
                            kBusy,
                            kLimit,
                            kIncompatible };
enum class SessionEnd { kUser,
                        kAgentClosed,
                        kAgentDisconnected,
                        kPermissionRevoked,
                        kFailure };
struct SessionRequest {
    std::string userId;
    std::string deviceId;
    wire::SessionKind kind = wire::SESSION_UNSPECIFIED;
    bool writeControl = false;
    bool preempt = true;
    std::uint32_t desktopInputContract = 0;
    bool agentInputContractSupported = false;
    std::string loginHash;
    std::int64_t grantRevision = 0;
    std::string approvalId;
};
struct IssuedChannel {
    std::string id;
    wire::ChannelPurpose purpose;
    std::string browserTicket;
    std::string agentTicket;
};
struct IssuedSession {
    std::string id;
    std::vector<IssuedChannel> channels;
    std::vector<std::string> preemptedSessions;
};
struct SessionInfo {
    std::string id;
    std::string deviceId;
    std::string state;
    std::string endReason;
};
struct ChannelAuthorization {
    std::string sessionId;
    std::string channelId;
    std::string userId;
    std::string deviceId;
    wire::SessionKind kind;
    wire::ChannelPurpose purpose;
    bool writeControl;
};
} // namespace ultraterimal::server
