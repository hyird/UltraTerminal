#include "ultraterimal/protocol/desktop.protocol.h"

#include <algorithm>
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
void DesktopChannelValidator::resetRefinement() noexcept {
    refined_ = false;
    refinementBytes_ = 0;
    refinementTiles_ = 0;
    refinementGrid_.reset();
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
            allowed = message.has_input_event() || message.has_quality_feedback() || message.has_request_refresh() || message.has_desktop_resize() || message.has_desktop_probe() || message.has_desktop_video_capabilities() || message.has_desktop_refinement_ack();
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
        contentFrame_ = 0;
        timestamp_ = 0;
        width_ = message.video_config().width();
        height_ = message.video_config().height();
        mode_ = Mode::kVideo;
        activeOptimizationContract_ = allowedOptimizationContract_ == 2 && message.video_config().desktop_optimization_contract() == 2 ? 2 : 0;
        activeContinuousFrames_ = activeOptimizationContract_ == 2 && allowedContinuousFrames_ && message.video_config().continuous_frames();
        refinementBatch_ = 0;
        resetRefinement();
        tiles_ = {};
        return true;
    }
    if (message.has_video_frame()) {
        const bool repeated = message.video_frame().repeated();
        const bool validBase = repeated ? activeContinuousFrames_ && contentFrame_ && message.base_frame_id() == contentFrame_ : !message.base_frame_id();
        if (mode_ != Mode::kVideo || message.generation() != generation_ || message.frame_id() <= frame_ || !validBase ||
            (frame_ == 0 && !message.video_frame().key_frame()) || (frame_ != 0 && message.video_frame().timestamp_microseconds() <= timestamp_)) {
            return false;
        }
        frame_ = message.frame_id();
        timestamp_ = message.video_frame().timestamp_microseconds();
        if (!repeated) {
            contentFrame_ = frame_;
            resetRefinement();
        }
        return true;
    }
    if (message.has_video_refinement()) {
        if (mode_ != Mode::kVideo || message.generation() != generation_ || !contentFrame_ || message.frame_id() != contentFrame_ || message.base_frame_id() != contentFrame_) {
            return false;
        }
        return acceptRefinement(message.video_refinement());
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
        contentFrame_ = frame_;
        resetRefinement();
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
        if (message.generation() != generation_) {
            activeOptimizationContract_ = 0;
            refinementBatch_ = 0;
            resetRefinement();
            tiles_ = {};
        }
        mode_ = Mode::kLossless;
        activeContinuousFrames_ = false;
        generation_ = message.generation();
        frame_ = message.frame_id();
        contentFrame_ = frame_;
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
bool DesktopChannelValidator::acceptRefinement(const wire::VideoRefinement& refinement) {
    if (activeOptimizationContract_ != 2) {
        if (refinement.batch_id() || refinement.kind() != wire::UNSPECIFIED || refined_ || !acceptPatches(refinement.patches())) {
            return false;
        }
        refined_ = true;
        return true;
    }
    if (!refinement.batch_id() || refinement.batch_id() <= refinementBatch_ ||
        (refinement.kind() != wire::STATIC_DETAIL && refinement.kind() != wire::DYNAMIC_DETAIL)) {
        return false;
    }
    std::array<std::size_t, 32> indices{};
    std::size_t count = 0;
    std::size_t bytes = 0;
    const std::size_t columns = (width_ + 127) / 128;
    const std::size_t rows = (height_ + 63) / 64;
    for (const auto& patch : refinement.patches()) {
        const auto& area = patch.destination();
        if (patch.cache_slot() || patch.png().empty() || area.x() >= width_ || area.y() >= height_ ||
            area.x() % 128 || area.y() % 64 || area.width() != std::min(128u, width_ - area.x()) || area.height() != std::min(64u, height_ - area.y())) {
            return false;
        }
        const std::size_t index = (area.y() / 64) * columns + area.x() / 128;
        if (refinementGrid_.test(index) || std::find(indices.begin(), indices.begin() + count, index) != indices.begin() + count) {
            return false;
        }
        indices[count++] = index;
        bytes += patch.png().size();
    }
    if (refinementTiles_ + count > std::min(std::size_t{2048}, columns * rows) || refinementBytes_ + bytes > 8 * 1024 * 1024) {
        return false;
    }
    // Commit only after all grid/resource checks, including the entire batch.
    for (std::size_t index = 0; index < count; ++index) {
        refinementGrid_.set(indices[index]);
    }
    refinementBatch_ = refinement.batch_id();
    refinementTiles_ += count;
    refinementBytes_ += bytes;
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
