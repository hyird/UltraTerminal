#include "ultraterimal/protocol/desktop.protocol.h"

#include <utility>

namespace ultraterimal::protocol {
DesktopChannelValidator::DesktopChannelValidator(DesktopChannelBinding binding) : binding_(std::move(binding)) {
    setOptimizationContract(binding_.optimizationContract);
}
void DesktopChannelValidator::setOptimizationContract(std::uint32_t contract) noexcept {
    allowedOptimizationContract_ = contract == 2 ? 2 : 0;
}
void DesktopChannelValidator::setContinuousFrames(bool allowed) noexcept {
    allowedContinuousFrames_ = allowed;
}
std::expected<void, DesktopMessageError> DesktopChannelValidator::accept(const wire::Envelope& message) {
    if (!validateMessage(message)) {
        return std::unexpected(DesktopMessageError::kMalformed);
    }
    if (message.session_id() != binding_.sessionId || message.channel_id() != binding_.channelId) {
        return std::unexpected(DesktopMessageError::kBinding);
    }
    if (message.sequence() <= sequence_) {
        return std::unexpected(DesktopMessageError::kReplay);
    }
    bool allowed = false;
    if (binding_.purpose == wire::DESKTOP_INPUT) {
        if (binding_.source == ChannelSource::kAgent) {
            allowed = message.has_cursor_update() || message.has_desktop_resize_result() || message.has_desktop_probe() || message.has_desktop_secure_attention_result() || message.has_desktop_pipeline_report();
        } else {
            allowed = message.has_input_event() || message.has_quality_feedback() || message.has_request_refresh() || message.has_desktop_resize() || message.has_desktop_probe() || message.has_desktop_video_capabilities();
            if ((message.has_input_event() || message.has_desktop_resize()) && !binding_.writeControl) {
                return std::unexpected(DesktopMessageError::kReadOnly);
            }
            if (message.has_desktop_probe() && (message.desktop_probe().reset_input() || message.desktop_probe().accepted_response_nonce())) {
                return std::unexpected(DesktopMessageError::kDirection);
            }
        }
    } else if (binding_.purpose == wire::DESKTOP_VIDEO && binding_.source == ChannelSource::kAgent) {
        allowed = message.has_video_config() || message.has_video_frame();
    }
    if (!allowed) {
        return std::unexpected(DesktopMessageError::kDirection);
    }
    if (binding_.purpose == wire::DESKTOP_VIDEO && !acceptFrame(message)) {
        return std::unexpected(DesktopMessageError::kFrameState);
    }
    sequence_ = message.sequence();
    return {};
}
bool DesktopChannelValidator::acceptFrame(const wire::Envelope& message) {
    if (!message.generation()) {
        return false;
    }
    if (message.has_video_config()) {
        if (message.generation() <= generation_ || message.frame_id() || message.base_frame_id()) {
            return false;
        }
        generation_ = message.generation();
        frame_ = 0;
        contentFrame_ = 0;
        timestamp_ = 0;
        activeOptimizationContract_ = allowedOptimizationContract_ == 2 && message.video_config().desktop_optimization_contract() == 2 ? 2 : 0;
        activeContinuousFrames_ = activeOptimizationContract_ == 2 && allowedContinuousFrames_ && message.video_config().continuous_frames();
        return true;
    }
    if (message.has_video_frame()) {
        const bool repeated = message.video_frame().repeated();
        const bool validBase = repeated ? activeContinuousFrames_ && contentFrame_ && message.base_frame_id() == contentFrame_ : !message.base_frame_id();
        if (!generation_ || message.generation() != generation_ || message.frame_id() <= frame_ || !validBase ||
            (frame_ == 0 && !message.video_frame().key_frame()) || (frame_ != 0 && message.video_frame().timestamp_microseconds() <= timestamp_)) {
            return false;
        }
        frame_ = message.frame_id();
        timestamp_ = message.video_frame().timestamp_microseconds();
        if (!repeated) {
            contentFrame_ = frame_;
        }
        return true;
    }
    return false;
}
} // namespace ultraterimal::protocol
