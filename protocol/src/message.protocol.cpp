#include "ultraterimal/protocol/message.protocol.h"

#include <algorithm>
#include <cstdint>

#include <google/protobuf/io/coded_stream.h>

#include "ultraterimal/protocol/desktop_failure.protocol.h"
#include "ultraterimal/protocol/update.protocol.h"

namespace ultraterimal::protocol {
namespace {
bool identifier(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool restartOperationId(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
bool restartDeviceId(std::string_view value) {
    return value.size() == 66 && value.substr(0, 2) == "d-" && restartOperationId(value.substr(2));
}
bool dimensions(std::uint32_t width, std::uint32_t height) {
    return width > 0 && height > 0 && width <= 16384 && height <= 16384;
}
bool rectangle(const wire::Rectangle& r) {
    return dimensions(r.width(), r.height()) && r.x() <= 16384 - r.width() && r.y() <= 16384 - r.height();
}
} // namespace
std::expected<void, MessageError> validateInputEvent(const wire::InputEvent& input) {
    bool valid = false;
    const bool noPosition = input.x() == 0 && input.y() == 0;
    switch (input.kind()) {
    case wire::InputEvent::POINTER_MOVE:
        valid = input.x() >= 0 && input.y() >= 0 && input.x() < 16384 && input.y() < 16384 && !input.code() && !input.pressed() && !input.wheel_delta();
        break;
    case wire::InputEvent::BUTTON:
        valid = noPosition && input.code() <= 4 && !input.wheel_delta();
        break;
    case wire::InputEvent::KEY: {
        const auto code = input.code();
        // Windows virtual-key values, with left/right modifiers. 0x10D is
        // the extended keypad Enter; packet/process and unassigned keys are excluded.
        const bool key = code == 3 || code == 8 || code == 9 || code == 12 || code == 13 ||
                         (code >= 0x13 && code <= 0x2f) || (code >= 0x30 && code <= 0x39) ||
                         (code >= 0x41 && code <= 0x5d) || code == 0x5f || (code >= 0x60 && code <= 0x87) ||
                         (code >= 0x90 && code <= 0x96) || (code >= 0xa0 && code <= 0xb7) ||
                         (code >= 0xba && code <= 0xc0) || (code >= 0xdb && code <= 0xdf) ||
                         code == 0xe1 || code == 0xe2 || code == 0xe6 || (code >= 0xe9 && code <= 0xfe) || code == 0x10d;
        valid = key && noPosition && !input.wheel_delta();
        break;
    }
    case wire::InputEvent::WHEEL:
        valid = noPosition && input.code() <= 1 && !input.pressed() && input.wheel_delta() != 0 && input.wheel_delta() >= -12000 && input.wheel_delta() <= 12000;
        break;
    case wire::InputEvent::RELEASE_ALL:
    case wire::InputEvent::SECURE_ATTENTION:
        valid = noPosition && !input.code() && !input.pressed() && !input.wheel_delta();
        break;
    default:
        break;
    }
    if (!valid) {
        return std::unexpected(MessageError::kInvalidField);
    }
    return {};
}
bool purposeMatches(wire::SessionKind kind, wire::ChannelPurpose purpose) noexcept {
    switch (kind) {
    case wire::DESKTOP:
        return purpose == wire::DESKTOP_INPUT || purpose == wire::DESKTOP_VIDEO;
    case wire::VNC:
        return purpose == wire::VNC_STREAM;
    case wire::SSH:
        return purpose == wire::SSH_STREAM;
    default:
        return false;
    }
}
std::expected<void, MessageError> validateMessage(const wire::Envelope& m) {
    if (m.has_desktop_probe() && (!m.desktop_probe().nonce() || m.desktop_probe().accepted_response_nonce() >= m.desktop_probe().nonce() || m.channel_id().empty() || m.generation() || m.frame_id() || m.base_frame_id())) {
        return std::unexpected(MessageError::kInvalidField);
    }
    if (m.ByteSizeLong() > kMaximumMessageBytes) {
        return std::unexpected(MessageError::kTooLarge);
    }
    if (m.protocol_version() != kVersion) {
        return std::unexpected(MessageError::kVersion);
    }
    if (m.body_case() == wire::Envelope::BODY_NOT_SET) {
        return std::unexpected(MessageError::kMissingBody);
    }
    if ((!m.session_id().empty() && !identifier(m.session_id())) || (!m.channel_id().empty() && !identifier(m.channel_id())) || m.sequence() == 0) {
        return std::unexpected(MessageError::kInvalidField);
    }
    const auto invalid = [] { return std::unexpected(MessageError::kInvalidField); };
    switch (m.body_case()) {
    case wire::Envelope::kHello:
        if (!identifier(m.hello().device_id()) || m.hello().agent_version().empty() || m.hello().agent_version().size() > 64 || !m.session_id().empty() || !m.channel_id().empty() || m.hello().capabilities().desktop_input_contract() > 2) {
            return invalid();
        }
        break;
    case wire::Envelope::kWelcome:
        if (m.welcome().heartbeat_seconds() < 1 || m.welcome().heartbeat_seconds() > 300 || !m.session_id().empty() || !m.channel_id().empty()) {
            return invalid();
        }
        break;
    case wire::Envelope::kHeartbeat:
    case wire::Envelope::kAgentLogs:
    case wire::Envelope::kAgentLogAck:
    case wire::Envelope::kAgentUpdate:
    case wire::Envelope::kWindowsLoginRequest:
    case wire::Envelope::kWindowsLoginResult:
    case wire::Envelope::kWindowsRestartRequest:
    case wire::Envelope::kWindowsRestartResult:
        if (!m.session_id().empty() || !m.channel_id().empty()) {
            return invalid();
        }
        break;
    default:
        if (!identifier(m.session_id())) {
            return invalid();
        }
        break;
    }
    switch (m.body_case()) {
    case wire::Envelope::kAgentUpdate:
        if (!validUpdate(m.agent_update()) || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kAgentLogs: {
        const auto& batch = m.agent_logs();
        if (!identifier(batch.runtime_id()) || batch.entries_size() < 1 || batch.entries_size() > 16 || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        std::uint64_t previous = 0;
        for (const auto& entry : batch.entries()) {
            if (entry.sequence() <= previous || !entry.unix_milliseconds() || !wire::AgentLog_Event_IsValid(entry.event()) || entry.event() == wire::AgentLog::UNSPECIFIED ||
                (!entry.session_id().empty() && !identifier(entry.session_id()))) {
                return invalid();
            }
            if (entry.has_desktop_fault() && (entry.event() != wire::AgentLog::DESKTOP_FAILED || entry.session_id().empty() || !validDesktopFault(entry.desktop_fault()))) {
                return invalid();
            }
            previous = entry.sequence();
        }
        break;
    }
    case wire::Envelope::kAgentLogAck:
        if (!m.agent_log_ack().sequence() || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kWindowsLoginRequest: {
        const auto& request = m.windows_login_request();
        const auto text = [](const std::string& value, std::size_t maximum) {
            return value.size() <= maximum && value.find('\0') == std::string::npos;
        };
        if (!request.request_id() || m.generation() || m.frame_id() || m.base_frame_id() ||
            !wire::WindowsLoginRequest_Action_IsValid(request.action()) || request.action() == wire::WindowsLoginRequest::UNSPECIFIED ||
            !text(request.username(), 512) || !text(request.domain(), 256) || !text(request.password(), 1024)) {
            return invalid();
        }
        if (request.action() != wire::WindowsLoginRequest::SAVE && (!request.username().empty() || !request.domain().empty() || !request.password().empty())) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kWindowsLoginResult: {
        const auto& result = m.windows_login_result();
        if (!result.request_id() || m.generation() || m.frame_id() || m.base_frame_id() ||
            result.username().size() > 512 || result.domain().size() > 256 ||
            result.username().find('\0') != std::string::npos || result.domain().find('\0') != std::string::npos ||
            (result.saved() && (!result.success() || result.username().empty())) ||
            (!result.saved() && (!result.username().empty() || !result.domain().empty()))) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kWindowsRestartRequest: {
        const auto& request = m.windows_restart_request();
        if (!restartOperationId(request.operation_id()) || !restartDeviceId(request.device_id()) ||
            !request.expires_at_unix_seconds() || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kWindowsRestartResult: {
        const auto& result = m.windows_restart_result();
        if (!restartOperationId(result.operation_id()) || !wire::WindowsRestartResult_Status_IsValid(result.status()) ||
            result.status() == wire::WindowsRestartResult::UNSPECIFIED || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kDesktopStart:
        if (!m.desktop_start().windows_session_id() || m.desktop_start().windows_session_id() == 0xffffffff || m.desktop_start().display_index() >= 64 ||
            (m.desktop_start().write_control() ? m.desktop_start().desktop_input_contract() != 2 : m.desktop_start().desktop_input_contract() != 0)) {
            return invalid();
        }
        [[fallthrough]];
    case wire::Envelope::kDesktopReady:
        if (m.sequence() != 1 || !m.channel_id().empty() || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kSessionOpen:
        if (!wire::SessionKind_IsValid(m.session_open().kind()) || m.session_open().kind() == wire::SESSION_UNSPECIFIED || !identifier(m.session_open().user_id()) || m.session_open().channels_size() != (m.session_open().kind() == wire::DESKTOP ? 2 : 1) ||
            ((m.session_open().kind() == wire::DESKTOP && m.session_open().write_control()) ? m.session_open().desktop_input_contract() != 2 : m.session_open().desktop_input_contract() != 0)) {
            return invalid();
        }
        for (int i = 0; i < m.session_open().channels_size(); ++i) {
            const auto& channel = m.session_open().channels(i);
            if (!identifier(channel.channel_id()) || !purposeMatches(m.session_open().kind(), channel.purpose()) || channel.ticket().empty() || channel.ticket().size() > 4096) {
                return invalid();
            }
            for (int j = 0; j < i; ++j) {
                const auto& previous = m.session_open().channels(j);
                if (previous.channel_id() == channel.channel_id() || previous.purpose() == channel.purpose()) {
                    return invalid();
                }
            }
        }
        break;
    case wire::Envelope::kChannelOpen:
        if (!identifier(m.channel_id()) || !wire::ChannelPurpose_IsValid(m.channel_open().purpose()) || m.channel_open().purpose() == wire::CHANNEL_UNSPECIFIED || m.channel_open().purpose() == wire::DEVICE_CONTROL || m.channel_open().ticket().empty() || m.channel_open().ticket().size() > 4096) {
            return invalid();
        }
        break;
    case wire::Envelope::kChannelReady:
        if (!identifier(m.channel_id()) || m.sequence() != 1 || m.generation() || m.frame_id() || m.base_frame_id() ||
            (m.channel_ready().purpose() != wire::DESKTOP_INPUT && m.channel_ready().purpose() != wire::DESKTOP_VIDEO)) {
            return invalid();
        }
        break;
    case wire::Envelope::kChannelData:
        if (!identifier(m.channel_id()) || m.channel_data().data().size() > 1024 * 1024) {
            return invalid();
        }
        break;
    case wire::Envelope::kVideoConfig:
        if (m.video_config().display_modes_size() > 128) {
            return invalid();
        }
        for (const auto& mode : m.video_config().display_modes()) {
            if (!dimensions(mode.width(), mode.height()) || mode.width() > 4096 || mode.height() > 2160 || mode.width() % 2 || mode.height() % 2) {
                return invalid();
            }
        }
        if (!m.generation() || !dimensions(m.video_config().width(), m.video_config().height()) || m.video_config().codec().empty() || m.video_config().codec().size() > 128) {
            return invalid();
        }
        break;
    case wire::Envelope::kVideoFrame: {
        const auto& frame = m.video_frame();
        if (!m.generation() || !m.frame_id() || frame.data().empty() ||
            (frame.has_target_frames_per_second() != frame.has_target_bitrate()) ||
            (frame.has_target_frames_per_second() &&
             (frame.target_frames_per_second() < 10 || frame.target_frames_per_second() > 60 ||
              frame.target_bitrate() < 64000 || frame.target_bitrate() > 100000000))) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kFullFrame:
        if (!m.generation() || !m.frame_id() || !dimensions(m.full_frame().width(), m.full_frame().height()) || m.full_frame().rgba().size() != static_cast<std::uint64_t>(m.full_frame().width()) * m.full_frame().height() * 4) {
            return invalid();
        }
        break;
    case wire::Envelope::kDiffBatch:
        if (!m.generation() || !m.base_frame_id() || m.frame_id() <= m.base_frame_id() || m.diff_batch().copies_size() + m.diff_batch().patches_size() > 4096) {
            return invalid();
        }
        for (const auto& copy : m.diff_batch().copies()) {
            if (!rectangle(copy.destination()) || copy.source_x() > 16384 - copy.destination().width() || copy.source_y() > 16384 - copy.destination().height()) {
                return invalid();
            }
        }
        for (const auto& patch : m.diff_batch().patches()) {
            if (!rectangle(patch.destination()) || patch.png().empty()) {
                return invalid();
            }
        }
        break;
    case wire::Envelope::kQualityFeedback:
        if (!wire::QualityFeedback_Preference_IsValid(m.quality_feedback().preference()) ||
            m.quality_feedback().maximum_pending_video_frames() > 128) {
            return invalid();
        }
        break;
    case wire::Envelope::kInputEvent:
        if (!m.generation() || m.frame_id() || m.base_frame_id() || !validateInputEvent(m.input_event())) {
            return invalid();
        }
        break;
    case wire::Envelope::kDesktopResize:
        if (!m.desktop_resize().request_id() || m.desktop_resize().width() < 16 || m.desktop_resize().height() < 16 ||
            m.desktop_resize().width() > 4096 || m.desktop_resize().height() > 2160 ||
            m.desktop_resize().width() % 2 || m.desktop_resize().height() % 2 || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kDesktopResizeResult:
        if (!m.desktop_resize_result().request_id() || m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kDesktopSecureAttentionResult:
        if (m.generation() || m.frame_id() || m.base_frame_id()) {
            return invalid();
        }
        break;
    case wire::Envelope::kCursorUpdate: {
        const auto& cursor = m.cursor_update();
        if (cursor.rgba().empty()) {
            if (cursor.width() || cursor.height() || cursor.hotspot_x() || cursor.hotspot_y()) {
                return invalid();
            }
        } else if (!dimensions(cursor.width(), cursor.height()) || cursor.width() > 1024 || cursor.height() > 1024 ||
                   cursor.hotspot_x() >= cursor.width() || cursor.hotspot_y() >= cursor.height() ||
                   cursor.rgba().size() != static_cast<std::uint64_t>(cursor.width()) * cursor.height() * 4) {
            return invalid();
        }
        break;
    }
    case wire::Envelope::kTerminalResize:
        if (!dimensions(m.terminal_resize().columns(), m.terminal_resize().rows()) || m.terminal_resize().columns() > 1000 || m.terminal_resize().rows() > 1000) {
            return invalid();
        }
        break;
    case wire::Envelope::kSessionError:
        if (m.session_error().code().empty() || m.session_error().code().size() > 128 || m.session_error().message().size() > 1024) {
            return invalid();
        }
        break;
    default:
        break;
    }
    return {};
}
std::expected<wire::Envelope, MessageError> decodeMessage(std::string_view bytes) {
    if (bytes.size() > kMaximumMessageBytes) {
        return std::unexpected(MessageError::kTooLarge);
    }
    google::protobuf::io::CodedInputStream input(reinterpret_cast<const std::uint8_t*>(bytes.data()), static_cast<int>(bytes.size()));
    input.SetTotalBytesLimit(static_cast<int>(kMaximumMessageBytes));
    input.SetRecursionLimit(32);
    wire::Envelope message;
    if (!message.ParseFromCodedStream(&input) || !input.ConsumedEntireMessage()) {
        return std::unexpected(MessageError::kMalformed);
    }
    if (auto result = validateMessage(message); !result) {
        return std::unexpected(result.error());
    }
    return message;
}
std::expected<std::string, MessageError> encodeMessage(const wire::Envelope& message) {
    if (auto result = validateMessage(message); !result) {
        return std::unexpected(result.error());
    }
    std::string bytes;
    if (!message.SerializeToString(&bytes)) {
        return std::unexpected(MessageError::kMalformed);
    }
    return bytes;
}
} // namespace ultraterimal::protocol
