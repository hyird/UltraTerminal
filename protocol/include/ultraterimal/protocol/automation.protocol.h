#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "ultraterimal/protocol/file.protocol.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::protocol {
inline constexpr std::size_t kMaximumExecCommandBytes = 16 * 1024;
inline constexpr std::size_t kMaximumExecOutputBytes = 48 * 1024;
inline constexpr std::size_t kDefaultExecOutputBytes = 32 * 1024;
inline constexpr std::uint32_t kMinimumExecTimeoutMs = 100;
inline constexpr std::uint32_t kDefaultExecTimeoutMs = 30000;
inline constexpr std::uint32_t kMaximumExecTimeoutMs = 120000;
inline constexpr std::size_t kMaximumSnapshotBytes = 4 * 1024 * 1024;
inline constexpr std::uint32_t kMaximumSnapshotDimension = 4096;
inline constexpr std::size_t kMaximumDesktopActionTextBytes = 4096;
inline constexpr int kMaximumDesktopActionKeys = 8;
inline constexpr std::int32_t kNormalizedCoordinateMax = 1000;

inline bool validVirtualKeyCode(std::uint32_t code) noexcept {
    // Windows virtual-key values, with left/right modifiers. 0x10D is
    // the extended keypad Enter; packet/process and unassigned keys are excluded.
    return code == 3 || code == 8 || code == 9 || code == 12 || code == 13 ||
           (code >= 0x13 && code <= 0x2f) || (code >= 0x30 && code <= 0x39) ||
           (code >= 0x41 && code <= 0x5d) || code == 0x5f || (code >= 0x60 && code <= 0x87) ||
           (code >= 0x90 && code <= 0x96) || (code >= 0xa0 && code <= 0xb7) ||
           (code >= 0xba && code <= 0xc0) || (code >= 0xdb && code <= 0xdf) ||
           code == 0xe1 || code == 0xe2 || code == 0xe6 || (code >= 0xe9 && code <= 0xfe) || code == 0x10d;
}

inline std::string_view execErrorCodeName(wire::ExecErrorCode code) noexcept {
    switch (code) {
    case wire::EXEC_ERROR_NONE:
        return "none";
    case wire::EXEC_ERROR_INVALID_REQUEST:
        return "invalid_request";
    case wire::EXEC_ERROR_TIMEOUT:
        return "timeout";
    case wire::EXEC_ERROR_SPAWN_FAILED:
        return "spawn_failed";
    case wire::EXEC_ERROR_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}

inline std::string_view desktopToolErrorCodeName(wire::DesktopToolErrorCode code) noexcept {
    switch (code) {
    case wire::DESKTOP_TOOL_ERROR_NONE:
        return "none";
    case wire::DESKTOP_TOOL_ERROR_UNSUPPORTED:
        return "unsupported";
    case wire::DESKTOP_TOOL_ERROR_UNAVAILABLE:
        return "unavailable";
    case wire::DESKTOP_TOOL_ERROR_BUSY:
        return "busy";
    case wire::DESKTOP_TOOL_ERROR_INVALID_REQUEST:
        return "invalid_request";
    case wire::DESKTOP_TOOL_ERROR_TIMEOUT:
        return "timeout";
    case wire::DESKTOP_TOOL_ERROR_FAILED:
        return "failed";
    default:
        return "failed";
    }
}

inline bool validExecCommandRequest(const wire::ExecCommandRequest& request) noexcept {
    if (!request.request_id() || request.command().empty() || request.command().size() > kMaximumExecCommandBytes ||
        request.command().find('\0') != std::string::npos || !validFilePathText(request.cwd(), true)) {
        return false;
    }
    if (request.timeout_ms() != 0 && (request.timeout_ms() < kMinimumExecTimeoutMs || request.timeout_ms() > kMaximumExecTimeoutMs)) {
        return false;
    }
    if (request.max_output_bytes() > kMaximumExecOutputBytes) {
        return false;
    }
    return true;
}

inline bool validExecCommandResult(const wire::ExecCommandResult& result) noexcept {
    if (!result.request_id() || !wire::ExecErrorCode_IsValid(result.error())) {
        return false;
    }
    const std::size_t total = result.stdout_data().size() + result.stderr_data().size();
    return total <= kMaximumExecOutputBytes;
}

inline bool validDesktopSnapshotStart(const wire::DesktopSnapshotStart& request) noexcept {
    if (!validTransferId(request.snapshot_id()) || request.display_index() >= 64) {
        return false;
    }
    if (request.max_width() > kMaximumSnapshotDimension || request.max_height() > kMaximumSnapshotDimension) {
        return false;
    }
    return true;
}

inline bool validDesktopSnapshotResult(const wire::DesktopSnapshotResult& result) noexcept {
    if (!validTransferId(result.snapshot_id()) || !wire::DesktopToolErrorCode_IsValid(result.error())) {
        return false;
    }
    if (result.png_data().size() > kMaximumSnapshotBytes) {
        return false;
    }
    if (result.success()) {
        if (!result.width() || !result.height() || result.width() > kMaximumSnapshotDimension || result.height() > kMaximumSnapshotDimension ||
            !result.display_width() || !result.display_height() || result.display_width() > 16384 || result.display_height() > 16384 ||
            result.png_data().empty()) {
            return false;
        }
    }
    return true;
}

inline bool validDesktopActionRequest(const wire::DesktopActionRequest& request) noexcept {
    if (!request.request_id() || request.display_index() >= 64 ||
        !wire::DesktopActionRequest_Action_IsValid(request.action()) || request.action() == wire::DesktopActionRequest::ACTION_UNSPECIFIED) {
        return false;
    }
    const std::int32_t maxCoord = request.normalized() ? kNormalizedCoordinateMax : 16383;
    const auto validCoord = [maxCoord](std::int32_t v) noexcept { return v >= 0 && v <= maxCoord; };
    if (!validCoord(request.x()) || !validCoord(request.y()) || !validCoord(request.to_x()) || !validCoord(request.to_y())) {
        return false;
    }
    if (request.button() > 4 || request.wheel_delta() < -12000 || request.wheel_delta() > 12000) {
        return false;
    }
    if (request.action() == wire::DesktopActionRequest::ACTION_SCROLL && request.wheel_delta() == 0) {
        return false;
    }
    if (request.keys_size() > kMaximumDesktopActionKeys) {
        return false;
    }
    for (const auto key : request.keys()) {
        if (!validVirtualKeyCode(key)) {
            return false;
        }
    }
    if ((request.action() == wire::DesktopActionRequest::ACTION_KEY && request.keys_size() != 1) ||
        (request.action() == wire::DesktopActionRequest::ACTION_HOTKEY && request.keys_size() == 0)) {
        return false;
    }
    if (request.text().size() > kMaximumDesktopActionTextBytes || request.text().find('\0') != std::string::npos) {
        return false;
    }
    if (request.action() == wire::DesktopActionRequest::ACTION_TYPE_TEXT && request.text().empty()) {
        return false;
    }
    return true;
}

inline bool validDesktopActionResult(const wire::DesktopActionResult& result) noexcept {
    return result.request_id() != 0 && wire::DesktopToolErrorCode_IsValid(result.error()) &&
           result.display_width() <= 16384 && result.display_height() <= 16384;
}

inline std::expected<wire::DesktopSnapshotResult, MessageError> decodeDesktopSnapshotResult(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumSnapshotBytes + 4096) {
        return std::unexpected(MessageError::kTooLarge);
    }
    wire::DesktopSnapshotResult result;
    if (!result.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(MessageError::kMalformed);
    }
    if (!validDesktopSnapshotResult(result)) {
        return std::unexpected(MessageError::kInvalidField);
    }
    return result;
}

inline std::expected<wire::DesktopToolMessage, MessageError> decodeDesktopToolMessage(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaximumSnapshotBytes + 16384) {
        return std::unexpected(MessageError::kTooLarge);
    }
    wire::DesktopToolMessage message;
    if (!message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(MessageError::kMalformed);
    }
    switch (message.body_case()) {
    case wire::DesktopToolMessage::kSnapshotRequest:
        if (!validDesktopSnapshotStart(message.snapshot_request())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return message;
    case wire::DesktopToolMessage::kSnapshotResult:
        if (!validDesktopSnapshotResult(message.snapshot_result())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return message;
    case wire::DesktopToolMessage::kActionRequest:
        if (!validDesktopActionRequest(message.action_request())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return message;
    case wire::DesktopToolMessage::kActionResult:
        if (!validDesktopActionResult(message.action_result())) {
            return std::unexpected(MessageError::kInvalidField);
        }
        return message;
    default:
        return std::unexpected(MessageError::kMissingBody);
    }
}
} // namespace ultraterimal::protocol
