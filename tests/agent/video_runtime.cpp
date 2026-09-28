#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <windows.h>

#include <ruvia/core/EventLoopAttachment.h>
#include <wels/codec_api.h>

#include "features/desktop/video.runtime.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace {
using namespace ultraterimal;
void require(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
void decode(const agent::DesktopVideoUpdate& update, const wire::VideoConfig& config) {
    require(update.frame.encode_microseconds() > 0, "Successful desktop frame omitted encoding duration");
    ISVCDecoder* decoder = nullptr;
    require(WelsCreateDecoder(&decoder) == 0 && decoder, "Cannot create video verifier");
    struct Owner {
        ISVCDecoder* decoder;
        ~Owner() {
            decoder->Uninitialize();
            WelsDestroyDecoder(decoder);
        }
    } owner{decoder};
    SDecodingParam settings{};
    settings.eEcActiveIdc = ERROR_CON_DISABLE;
    settings.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    require(decoder->Initialize(&settings) == 0, "Cannot initialize video verifier");
    unsigned char* planes[3]{};
    SBufferInfo information{};
    information.uiInBsTimeStamp = update.frame.timestamp_microseconds();
    const auto& bytes = update.frame.data();
    const auto result = decoder->DecodeFrameNoDelay(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), planes, &information);
    require(result == dsErrorFree && information.iBufferStatus == 1, "Desktop key frame is not independently decodable");
    require(information.UsrData.sSystemBuffer.iWidth == static_cast<int>(config.width()) && information.UsrData.sSystemBuffer.iHeight == static_cast<int>(config.height()), "Desktop dimensions changed in encoding");
    require(information.uiOutYuvTimeStamp == update.frame.timestamp_microseconds(), "Desktop presentation timestamp changed");
}
asio::awaitable<void> verify(agent::DesktopVideoRuntime& video, asio::steady_timer& pulse, unsigned& ticks) {
    std::optional<agent::DesktopVideoUpdate> first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!first && std::chrono::steady_clock::now() < deadline) {
        first = co_await video.next();
        if (first && first->frame.data().empty()) {
            first.reset();
        }
    }
    require(first && first->configuration && first->generation == 1 && first->frameId == 1 && first->frame.key_frame(), "Missing first configured key frame");
    require(first->cursor.has_value(), "Missing initial cursor snapshot");
    require(ticks > 0, "Capture/encoding blocked the Ruvia event loop");
    const auto configuration = *first->configuration;
    protocol::DesktopChannelValidator validator({"runtime", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false});
    wire::Envelope message;
    message.set_protocol_version(protocol::kVersion);
    message.set_session_id("runtime");
    message.set_channel_id("video");
    message.set_sequence(2);
    message.set_generation(first->generation);
    *message.mutable_video_config() = configuration;
    require(bool(validator.accept(message)), "Runtime configuration violates channel protocol");
    message.set_sequence(3);
    message.set_frame_id(first->frameId);
    *message.mutable_video_frame() = first->frame;
    require(bool(validator.accept(message)), "Runtime first frame violates channel protocol");
    decode(*first, configuration);
    // A real browser acknowledges displayed frames. Without this ACK a large
    // first IDR correctly consumes the pacing window and blocks the refresh.
    wire::QualityFeedback firstDisplayed;
    firstDisplayed.set_displayed_generation(first->generation);
    firstDisplayed.set_displayed_frame(first->frameId);
    firstDisplayed.set_preference(wire::QualityFeedback::SHARP);
    video.feedback(firstDisplayed);

    // No test window change is needed: refresh must also work on a static desktop.
    auto refreshed = co_await video.next(true);
    require(refreshed && !refreshed->configuration && refreshed->generation == 1 && refreshed->frameId == 2 && refreshed->frame.key_frame(), "Refresh did not preserve the generation and produce an IDR");
    require(refreshed->cursor.has_value(), "Refresh did not resend the cursor snapshot");
    require(refreshed->frame.timestamp_microseconds() > first->frame.timestamp_microseconds(), "Frame timestamps are not increasing");
    message.set_sequence(4);
    message.set_frame_id(refreshed->frameId);
    *message.mutable_video_frame() = refreshed->frame;
    require(bool(validator.accept(message)), "Runtime refresh violates channel protocol");
    decode(*refreshed, configuration);

    wire::QualityFeedback quality;
    quality.set_displayed_generation(refreshed->generation);
    quality.set_displayed_frame(refreshed->frameId);
    quality.set_preference(wire::QualityFeedback::BALANCED);
    video.feedback(quality);
    const auto sharper = co_await video.next();
    require(sharper && sharper->frame.key_frame() && sharper->generation == refreshed->generation &&
                sharper->frameId == refreshed->frameId + 1,
            "Quality change did not refresh the static desktop without reconnecting");
    decode(*sharper, configuration);

    std::optional<agent::DesktopVideoUpdate> rebuilt;
    auto previousTimestamp = sharper->frame.timestamp_microseconds();
    // Exercise repeated real DXGI/codec teardown rather than only one reset.
    // Each new baseline must work with a fresh independent decoder.
    for (std::uint64_t generation = 2; generation <= 13; ++generation) {
        video.invalidate();
        rebuilt.reset();
        const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!rebuilt && std::chrono::steady_clock::now() < recoveryDeadline) {
            rebuilt = co_await video.next(true);
            if (rebuilt && rebuilt->frame.data().empty()) {
                rebuilt.reset();
            }
        }
        require(rebuilt && rebuilt->configuration && rebuilt->generation == generation && rebuilt->frameId == 1 && rebuilt->frame.key_frame(), "Invalidation did not rebuild the capture baseline");
        require(rebuilt->cursor.has_value(), "Rebuilt capture omitted its cursor baseline");
        require(rebuilt->frame.timestamp_microseconds() > previousTimestamp, "Capture rebuild reset the presentation clock");
        previousTimestamp = rebuilt->frame.timestamp_microseconds();
        message.set_sequence(message.sequence() + 1);
        message.set_generation(rebuilt->generation);
        message.clear_frame_id();
        *message.mutable_video_config() = *rebuilt->configuration;
        require(bool(validator.accept(message)), "Rebuilt configuration violates channel protocol");
        message.set_sequence(message.sequence() + 1);
        message.set_frame_id(1);
        *message.mutable_video_frame() = rebuilt->frame;
        require(bool(validator.accept(message)), "Rebuilt frame violates channel protocol");
        decode(*rebuilt, *rebuilt->configuration);
    }

    wire::QualityFeedback feedback;
    feedback.set_displayed_generation(rebuilt->generation);
    feedback.set_displayed_frame(rebuilt->frameId);
    feedback.set_decode_microseconds(1000);
    video.feedback(feedback);
    std::uint64_t lastSent = rebuilt->frameId;
    bool blocked = false;
    for (unsigned i = 0; i < 20; ++i) {
        auto update = co_await video.next(true);
        if (update && !update->frame.data().empty()) {
            lastSent = update->frameId;
        } else {
            require(update && update->cursor, "Congestion stopped cursor refresh");
            blocked = true;
            break;
        }
    }
    require(blocked && lastSent <= 17, "Missing display ACKs did not bound encoded frames");
    feedback.set_displayed_frame(lastSent);
    video.feedback(feedback);
    auto resumed = co_await video.next(true);
    require(resumed && resumed->frameId > lastSent && resumed->frame.key_frame(), "Display ACK did not resume encoding");
    decode(*resumed, *rebuilt->configuration);
    feedback.set_displayed_frame(resumed->frameId);
    feedback.set_background(true);
    video.feedback(feedback);
    for (unsigned i = 0; i < 3; ++i) {
        const auto hidden = co_await video.next(true);
        require(!hidden || hidden->frame.data().empty(), "Background desktop kept encoding video");
    }
    // Returning to a static desktop uses the same displayed-frame ACK.
    asio::steady_timer foreground(pulse.get_executor());
    foreground.expires_after(std::chrono::milliseconds(20));
    foreground.async_wait([&video, &feedback](std::error_code error) {
        if (!error) {
            feedback.set_background(false);
            video.feedback(feedback);
        }
    });
    const auto visible = co_await video.next();
    require(visible && visible->frameId == resumed->frameId + 1 && visible->generation == resumed->generation,
            "Foreground desktop did not resume its existing video baseline");
    decode(*visible, *rebuilt->configuration);

    require(!(co_await video.resize(0, 1080)), "Invalid display size accepted");
    require(!(co_await video.resize(17, 16)), "Odd display size accepted");
    require(!(co_await video.resize(16, 16)), "Unsupported display mode accepted");
    require(co_await video.resize(configuration.width(), configuration.height()), "Current display mode could not be reapplied");
    std::optional<agent::DesktopVideoUpdate> resized;
    const auto resizeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!resized && std::chrono::steady_clock::now() < resizeDeadline) {
        resized = co_await video.next(true);
        if (resized && resized->frame.data().empty()) {
            resized.reset();
        }
    }
    require(resized && resized->configuration && resized->generation == visible->generation + 1 && resized->frameId == 1,
            "Display resize did not rebuild the video generation");
    require(resized->configuration->display_modes_size() > 0, "Driver display modes were not advertised");
    decode(*resized, *resized->configuration);

    // Stop while next() is suspended, then await its completion before teardown.
    pulse.cancel();
    const auto stopRequested = std::make_shared<bool>(false);
    pulse.expires_after(std::chrono::milliseconds(1));
    pulse.async_wait([&video, stopRequested](std::error_code error) {
        if (!error) {
            *stopRequested = true;
            video.stop();
        }
    });
    const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // A cursor-only update can finish before the 1 ms timer. Only reject an
    // update returned after the stop callback actually ran; continue awaiting
    // capture until the cancellation overlaps a suspended next().
    while (!*stopRequested) {
        require(std::chrono::steady_clock::now() < stopDeadline, "Stop callback did not run");
        const auto stopped = co_await video.next();
        require(!*stopRequested || !stopped, "Stopped video runtime published a frame");
    }
    require(!(co_await video.next(true)), "Stopped video runtime restarted capture");
}
} // namespace
int main() {
    try {
        require(SetThreadExecutionState(ES_DISPLAY_REQUIRED) != 0, "Cannot wake test display");
        asio::io_context io;
        auto attachment = ruvia::attachEventLoop(io);
        agent::DesktopVideoRuntime video(attachment.loop(), 0);
        asio::steady_timer pulse(io);
        unsigned ticks = 0;
        std::function<void(std::error_code)> tick;
        tick = [&](std::error_code error) {
            if (!error) {
                ++ticks;
                pulse.expires_after(std::chrono::milliseconds(1));
                pulse.async_wait(tick);
            }
        };
        pulse.expires_after(std::chrono::milliseconds(1));
        pulse.async_wait(tick);
        std::exception_ptr failure;
        asio::co_spawn(io, verify(video, pulse, ticks), [&](std::exception_ptr error) {
            failure = error;
            pulse.cancel();
            video.stop();
            attachment.stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Real desktop capture/encode, twelve baseline rebuilds, independent refresh decode, event-loop responsiveness and stop passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
