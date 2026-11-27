#pragma once
#include <cstdint>

#include "common/http.h"

namespace ultraterimal::server {
RUVIA_MODEL(SessionCreateRequest,
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string, RUVIA_NOT_EMPTY("device_id required"), RUVIA_MAX_BYTES(128, "device_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(kind, ruvia::string, RUVIA_NOT_EMPTY("kind required"), RUVIA_MAX_BYTES(16, "kind too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_REQUIRED_FIELD(write_control, bool),
            RUVIA_OPTIONAL_FIELD(preempt, bool),
            RUVIA_OPTIONAL_FIELD(desktop_input_contract, std::int64_t),
            RUVIA_OPTIONAL_FIELD(approval_id, ruvia::string, RUVIA_NOT_EMPTY("approval_id required"), RUVIA_MAX_BYTES(32, "approval_id too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(verification_code, ruvia::string, RUVIA_MAX_BYTES(6, "verification_code too long"), RUVIA_CUSTOM(noNul, "invalid character")),
            RUVIA_OPTIONAL_FIELD(reconnect_session_id, ruvia::string, RUVIA_MAX_BYTES(128, "reconnect_session_id too long"), RUVIA_CUSTOM(noNul, "invalid character")));
RUVIA_MODEL(SessionChannelItem,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(purpose, std::int32_t),
            RUVIA_REQUIRED_FIELD(ticket, ruvia::string));
RUVIA_MODEL(SessionCreateResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(state, ruvia::string),
            RUVIA_REQUIRED_FIELD(expires_in, std::int32_t),
            RUVIA_REQUIRED_FIELD(channels, ruvia::array<SessionChannelItem>));
RUVIA_MODEL(SessionInfoResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(device_id, ruvia::string),
            RUVIA_REQUIRED_FIELD(state, ruvia::string),
            RUVIA_REQUIRED_FIELD(end_reason, ruvia::string));
RUVIA_MODEL(SessionCloseResponse,
            RUVIA_REQUIRED_FIELD(id, ruvia::string),
            RUVIA_REQUIRED_FIELD(state, ruvia::string));
} // namespace ultraterimal::server
