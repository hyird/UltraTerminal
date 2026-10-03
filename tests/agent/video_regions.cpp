#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <ruvia/core/EventLoopAttachment.h>

#include "features/desktop/video.runtime.h"
#include "platform/windows/capture.transport.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::agent {
// Deterministic DXGI boundary double. Production runtime, pacing, PNG encoder,
// cache, software/hardware selector and protocol validation remain unchanged.
struct DesktopCapture::State {
    std::uint64_t ticks = 0;
};
DesktopCapture::DesktopCapture(std::uint32_t) : state_(std::make_unique<State>()) {}
DesktopCapture::~DesktopCapture() = default;
std::optional<CapturedFrame> DesktopCapture::nextFrame(std::chrono::milliseconds, std::vector<std::uint8_t>*, CaptureTiming*, bool, bool readPixels, bool) {
    CapturedFrame frame;
    frame.width = 512;
    frame.height = 256;
    frame.rotation = 1;
    frame.captureTicks = ++state_->ticks;
    frame.damageKnown = true;
    wire::Rectangle area;
    area.set_width(128);
    area.set_height(64);
    frame.dirty.push_back(area);
    if (readPixels) {
        frame.bgra.assign(512 * 256 * 4, 255);
        for (unsigned y = 0; y < 64; ++y) {
            for (unsigned x = 0; x < 128; ++x) {
                frame.bgra[(y * 512 + x) * 4] = state_->ticks % 2 ? 32 : 208;
            }
        }
    }
    return frame;
}
ID3D11Device* DesktopCapture::device() const { return nullptr; }
ID3D11Texture2D* DesktopCapture::texture() const { return nullptr; }
void DesktopCapture::readTexture(CapturedFrame&) { throw std::runtime_error("Unexpected GPU readback in capture double"); }
std::vector<DesktopCapture::DisplayMode> DesktopCapture::displayModes() const { return {{512, 256}}; }
bool DesktopCapture::resize(std::uint32_t, std::uint32_t) { return false; }
} // namespace ultraterimal::agent
namespace {
using namespace ultraterimal;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
asio::awaitable<void> verify(agent::DesktopVideoRuntime& video) {
    wire::DesktopVideoCapabilities capabilities;
    capabilities.add_codecs(wire::AVC);
    capabilities.set_region_updates(true);
    capabilities.set_tile_cache(true);
    video.capabilities(capabilities);
    protocol::DesktopChannelValidator validator({"regions", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false});
    wire::Envelope message;
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("regions");
    message.set_channel_id("video");
    std::uint64_t sequence = 1;
    unsigned regions = 0, references = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (regions < 16 && std::chrono::steady_clock::now() < until) {
        const auto update = co_await video.next();
        if (!update) {
            continue;
        }
        message.set_generation(update->generation);
        if (update->configuration) {
            message.set_sequence(++sequence);
            message.set_frame_id(0);
            message.set_base_frame_id(0);
            *message.mutable_video_config() = *update->configuration;
            require(validator.accept(message).has_value(), "Runtime region config invalid");
        }
        if (update->region || !update->frame.data().empty()) {
            message.set_sequence(++sequence);
            message.set_frame_id(update->frameId);
            message.set_base_frame_id(update->baseFrameId);
            if (update->region) {
                *message.mutable_desktop_region_update() = *update->region;
                require(update->region->patches_size() == 1 && update->region->copies_size() == 0, "Small dirty update became whole-frame traffic");
                for (const auto& patch : update->region->patches()) {
                    references += patch.png().empty();
                }
                ++regions;
            } else {
                *message.mutable_video_frame() = update->frame;
            }
            require(validator.accept(message).has_value(), "Runtime update violated composed baseline");
            wire::QualityFeedback feedback;
            feedback.set_displayed_generation(update->generation);
            feedback.set_displayed_frame(update->frameId);
            feedback.set_decode_microseconds(1000);
            video.feedback(feedback);
        }
    }
    require(regions == 16 && references > 0, "Region updates or cache hits did not reach the receiver");
    const auto refreshed = co_await video.next(true);
    require(refreshed && !refreshed->frame.data().empty() && refreshed->frame.key_frame(), "Explicit refresh did not return to a video baseline");
    message.set_sequence(++sequence);
    message.set_frame_id(refreshed->frameId);
    message.set_base_frame_id(0);
    *message.mutable_video_frame() = refreshed->frame;
    require(validator.accept(message).has_value(), "Video after region updates lost codec continuity");
    std::cout << "Runtime delivered " << regions << " regions and " << references << " cached tiles; video refresh retained continuity\n";
    video.stop();
}
} // namespace
int main() {
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    agent::DesktopVideoRuntime video(attachment.loop(), 0);
    std::exception_ptr failure;
    asio::co_spawn(io, verify(video), [&](std::exception_ptr error) { failure = error; video.stop(); attachment.stop(); });
    attachment.run();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
        return 1;
    }
    return 0;
}
