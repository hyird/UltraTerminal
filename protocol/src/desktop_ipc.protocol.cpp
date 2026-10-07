#include "ultraterimal/protocol/desktop_ipc.protocol.h"

#include <utility>

namespace ultraterimal::protocol {
DesktopPipeValidator::DesktopPipeValidator(DesktopPipeBinding binding)
    : binding_(std::move(binding)),
      input_({binding_.sessionId, "input", wire::DESKTOP_INPUT, binding_.source == DesktopPipeSource::kService ? ChannelSource::kBrowser : ChannelSource::kAgent, binding_.writeControl, binding_.optimizationContract}),
      video_({binding_.sessionId, "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, binding_.writeControl, binding_.optimizationContract}) {}
void DesktopPipeValidator::setOptimizationContract(std::uint32_t contract) noexcept {
    input_.setOptimizationContract(contract);
    video_.setOptimizationContract(contract);
}
void DesktopPipeValidator::setContinuousFrames(bool allowed) noexcept {
    video_.setContinuousFrames(allowed);
}
std::expected<void, DesktopMessageError> DesktopPipeValidator::accept(const wire::Envelope& message) {
    if (!validateMessage(message)) {
        return std::unexpected(DesktopMessageError::kMalformed);
    }
    if (message.session_id() != binding_.sessionId) {
        return std::unexpected(DesktopMessageError::kBinding);
    }
    if (ended_ || message.sequence() <= sequence_) {
        return std::unexpected(DesktopMessageError::kReplay);
    }
    if (message.has_session_close() || message.has_session_error()) {
        if (!message.channel_id().empty() || message.generation() || message.frame_id() || message.base_frame_id() ||
            (!started_ && message.sequence() != 1) || message.ByteSizeLong() > kMaximumControlBytes) {
            return std::unexpected(DesktopMessageError::kMalformed);
        }
        ended_ = true;
    } else if (!started_) {
        if (binding_.source == DesktopPipeSource::kService) {
            if (!message.has_desktop_start() || message.desktop_start().windows_session_id() != binding_.windowsSessionId || message.desktop_start().write_control() != binding_.writeControl) {
                return std::unexpected(DesktopMessageError::kBinding);
            }
        } else if (!message.has_desktop_ready()) {
            return std::unexpected(DesktopMessageError::kDirection);
        }
        started_ = true;
    } else {
        if (message.channel_id() == "input") {
            // Only the authenticated desktop child may ask its parent service to
            // dispatch SAS, after validating the current input generation.
            if (binding_.source == DesktopPipeSource::kDesktop && message.has_input_event() &&
                message.input_event().kind() == wire::InputEvent::SECURE_ATTENTION) {
                if (!binding_.writeControl) {
                    return std::unexpected(DesktopMessageError::kReadOnly);
                }
                sequence_ = message.sequence();
                return {};
            }
            if (auto result = input_.accept(message); !result) {
                return result;
            }
        } else if (message.channel_id() == "video" && binding_.source == DesktopPipeSource::kDesktop) {
            if (auto result = video_.accept(message); !result) {
                return result;
            }
        } else {
            return std::unexpected(DesktopMessageError::kDirection);
        }
    }
    sequence_ = message.sequence();
    return {};
}
} // namespace ultraterimal::protocol
