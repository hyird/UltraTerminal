#pragma once

#include <asio/awaitable.hpp>
#include <utility>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Pixel portion of the Worker's single IPC writer. Borrow the completed update
// only for this awaited operation; the caller retains configuration/cursor and
// refresh/lifecycle ownership. Also accepts a fake runtime's completed update.
template <typename Update, typename Send>
asio::awaitable<void> writeDesktopPixels(Update& update, Send send) {
    if (update.frame.data().empty()) {
        if (update.region) {
            wire::Envelope region;
            region.set_channel_id("video");
            region.set_generation(update.generation);
            region.set_frame_id(update.frameId);
            region.set_base_frame_id(update.baseFrameId);
            *region.mutable_desktop_region_update() = std::move(*update.region);
            co_await send(std::move(region));
        }
    } else {
        wire::Envelope frame;
        frame.set_channel_id("video");
        frame.set_generation(update.generation);
        frame.set_frame_id(update.frameId);
        frame.set_base_frame_id(update.baseFrameId);
        *frame.mutable_video_frame() = std::move(update.frame);
        co_await send(std::move(frame));
    }
    // Keep the complete compute-generated body. Dynamic detail attached to a
    // video frame must follow that frame, just as detail follows a region update.
    if (update.refinement) {
        wire::Envelope refinement;
        refinement.set_channel_id("video");
        refinement.set_generation(update.generation);
        refinement.set_frame_id(update.refinementFrameId);
        refinement.set_base_frame_id(update.refinementFrameId);
        *refinement.mutable_video_refinement() = std::move(*update.refinement);
        co_await send(std::move(refinement));
    }
}
} // namespace ultraterimal::agent
