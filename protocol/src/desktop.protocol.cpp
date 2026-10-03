#include "ultraterimal/protocol/desktop.protocol.h"

#include <utility>

namespace ultraterimal::protocol {
DesktopChannelValidator::DesktopChannelValidator(DesktopChannelBinding binding) : binding_(std::move(binding)) {}
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
            allowed = message.has_cursor_update() || message.has_desktop_resize_result() || message.has_desktop_probe() || message.has_desktop_secure_attention_result();
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
        allowed = message.has_video_config() || message.has_video_frame() || message.has_full_frame() || message.has_diff_batch() || message.has_video_refinement() || message.has_desktop_region_update();
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
        timestamp_ = 0;
        width_ = message.video_config().width();
        height_ = message.video_config().height();
        mode_ = Mode::kVideo;
        refined_ = false;
        tiles_ = {};
        return true;
    }
    if (message.has_video_frame()) {
        if (mode_ != Mode::kVideo || message.generation() != generation_ || message.frame_id() <= frame_ || message.base_frame_id() ||
            (frame_ == 0 && !message.video_frame().key_frame()) || (frame_ != 0 && message.video_frame().timestamp_microseconds() <= timestamp_)) {
            return false;
        }
        frame_ = message.frame_id();
        timestamp_ = message.video_frame().timestamp_microseconds();
        refined_ = false;
        return true;
    }
    if (message.has_video_refinement()) {
        if (mode_ != Mode::kVideo || refined_ || message.generation() != generation_ || !frame_ || message.frame_id() != frame_ || message.base_frame_id() != frame_) {
            return false;
        }
        if (!acceptPatches(message.video_refinement().patches())) {
            return false;
        }
        refined_ = true;
        return true;
    }
    if (message.has_desktop_region_update()) {
        if (mode_ != Mode::kVideo || message.generation() != generation_ || !frame_ || message.base_frame_id() != frame_ || message.frame_id() <= frame_) {
            return false;
        }
        for (const auto& copy : message.desktop_region_update().copies()) {
            const auto& area = copy.destination();
            if (area.width() > width_ || area.height() > height_ || area.x() > width_ - area.width() || area.y() > height_ - area.height() ||
                copy.source_x() > width_ - area.width() || copy.source_y() > height_ - area.height()) {
                return false;
            }
        }
        if (!acceptPatches(message.desktop_region_update().patches())) {
            return false;
        }
        frame_ = message.frame_id();
        refined_ = false;
        return true;
    }
    if (message.has_full_frame()) {
        if (!message.frame_id() || message.base_frame_id() || message.generation() < generation_) {
            return false;
        }
        const auto& full = message.full_frame();
        if (message.generation() == generation_ && (mode_ != Mode::kLossless || full.width() != width_ || full.height() != height_ || message.frame_id() <= frame_)) {
            return false;
        }
        mode_ = Mode::kLossless;
        generation_ = message.generation();
        frame_ = message.frame_id();
        width_ = full.width();
        height_ = full.height();
        return true;
    }
    if (mode_ != Mode::kLossless || message.generation() != generation_ || message.base_frame_id() != frame_) {
        return false;
    }
    const auto fits = [&](const wire::Rectangle& rectangle) {
        return rectangle.width() <= width_ && rectangle.height() <= height_ && rectangle.x() <= width_ - rectangle.width() && rectangle.y() <= height_ - rectangle.height();
    };
    for (const auto& copy : message.diff_batch().copies()) {
        if (!fits(copy.destination()) || copy.source_x() > width_ - copy.destination().width() || copy.source_y() > height_ - copy.destination().height()) {
            return false;
        }
    }
    for (const auto& patch : message.diff_batch().patches()) {
        if (!fits(patch.destination())) {
            return false;
        }
    }
    frame_ = message.frame_id();
    return true;
}
bool DesktopChannelValidator::acceptPatches(const google::protobuf::RepeatedPtrField<wire::PatchRect>& patches) {
    auto staged = tiles_;
    for (const auto& patch : patches) {
        const auto& area = patch.destination();
        if (area.width() > width_ || area.height() > height_ || area.x() > width_ - area.width() || area.y() > height_ - area.height()) {
            return false;
        }
        if (patch.cache_slot()) {
            const auto dimensions = std::pair{area.width(), area.height()};
            auto& slot = staged[patch.cache_slot() - 1];
            if (patch.png().empty() && slot != dimensions) {
                return false;
            }
            slot = dimensions;
        }
    }
    tiles_ = staged;
    return true;
}
} // namespace ultraterimal::protocol
