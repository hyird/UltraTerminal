#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <blocking_pool.h>
#include <chrono>
#include <event_loop.h>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <windows.h>

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
// Resolution-changing coverage is opt-in and only runs on a single display.
// Restore the original mode even when capture recovery or decoding fails.
struct DisplayRestore {
    DEVMODEW original{};
    explicit DisplayRestore(bool enabled) {
        if (enabled) {
            require(GetSystemMetrics(SM_CMONITORS) == 1, "Resize test requires exactly one monitor");
            original.dmSize = sizeof(original);
            require(EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &original), "Cannot save original display mode");
        }
    }
    void restore() {
        if (!original.dmSize) {
            return;
        }
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current) && current.dmPelsWidth == original.dmPelsWidth && current.dmPelsHeight == original.dmPelsHeight) {
            return;
        }
        require(ChangeDisplaySettingsExW(nullptr, &original, nullptr, CDS_UPDATEREGISTRY | CDS_GLOBAL, nullptr) == DISP_CHANGE_SUCCESSFUL, "Cannot restore original display mode");
    }
    ~DisplayRestore() {
        try {
            restore();
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
    }
};
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
    if (result != dsErrorFree || information.iBufferStatus != 1) {
        throw std::runtime_error("Desktop key frame is not independently decodable: generation=" + std::to_string(update.generation) + " frame=" + std::to_string(update.frameId) + " codec=" + config.codec() + " result=" + std::to_string(result) + " output=" + std::to_string(information.iBufferStatus));
    }
    require(information.UsrData.sSystemBuffer.iWidth == static_cast<int>(config.width()) && information.UsrData.sSystemBuffer.iHeight == static_cast<int>(config.height()), "Desktop dimensions changed in encoding");
    require(information.uiOutYuvTimeStamp == update.frame.timestamp_microseconds(), "Desktop presentation timestamp changed");
}
asio::awaitable<void> verify(agent::DesktopVideoRuntime& video, asio::steady_timer& pulse, unsigned& ticks, bool changeResolution, bool optimization) {
    if (optimization) {
        wire::DesktopVideoCapabilities capabilities;
        capabilities.add_codecs(wire::AVC);
        capabilities.set_desktop_optimization_contract(2);
        video.capabilities(capabilities);
    }
    std::optional<agent::DesktopVideoUpdate> first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!first && std::chrono::steady_clock::now() < deadline) {
        first = co_await video.next();
        if (first && first->frame.data().empty()) {
            first.reset();
        }
    }
    require(first && first->configuration && first->generation == 1 && first->frameId == 1 && first->frame.key_frame(), "Missing first configured key frame");
    const auto diagnostics = video.diagnostics();
    std::cerr << "first_numeric captureCalls=" << diagnostics.captureCalls << " pixelUpdates=" << diagnostics.pixelUpdates
              << " cursorOnly=" << diagnostics.cursorOnlyUpdates << " timeouts=" << diagnostics.captureTimeouts
              << " returnedSnapshots=" << diagnostics.returnedSnapshots << " encodedFrames=" << diagnostics.encodedFrames << " encodeCalls=" << diagnostics.videoEncodeCalls
              << " published=" << diagnostics.publishedVideoFrames << " rejectedPrivateFrames=" << diagnostics.rejectedPrivateVideoFrames << " rejectedJobs=" << diagnostics.rejectedPrivateJobs
              << " frame=" << first->frameId << " generation=" << first->generation << " jobRevision=" << diagnostics.lastJobRevision << " ownerRevision=" << diagnostics.ownerRevision
              << " lastAcquired=" << diagnostics.lastJobAcquired
              << " acquireEpoch=" << diagnostics.lastAcquireEpoch << " acquireSerial=" << diagnostics.lastAcquireSerial << " acquirePresent=" << diagnostics.lastAcquirePresentTicks
              << " outputPresent=" << first->sourcePresentTicks << '\n';
    // DXGI may return an initial image without a new present timestamp. It is
    // a real acquired snapshot/decoded baseline, not a fabricated pixel event.
    // Old private VIDEO updates count production, not owner publication.
    const bool initialSnapshot = optimization && first->sourcePresentTicks == 0 && diagnostics.lastAcquirePresentTicks == 0 && diagnostics.cursorOnlyUpdates >= 1;
    require(diagnostics.captureCalls >= diagnostics.pixelUpdates && (diagnostics.pixelUpdates >= 1 || initialSnapshot), "Capture diagnostics lost the first source update/snapshot");
    require(diagnostics.captureCalls >= diagnostics.returnedSnapshots && diagnostics.returnedSnapshots >= 1 && diagnostics.lastAcquireEpoch != 0 && diagnostics.lastAcquireSerial != 0,
            "First baseline lacked an actual retained Acquire identity");
    require(diagnostics.publishedVideoFrames == 1 && diagnostics.encodedFrames == diagnostics.publishedVideoFrames + diagnostics.rejectedPrivateVideoFrames && (optimization || diagnostics.encodedFrames == 1),
            "First publication lost private/committed VIDEO accounting");
    require(diagnostics.lastJobRevision == diagnostics.ownerRevision, "First baseline used a rejected job revision");
    require(diagnostics.pixelCaptureGaps <= diagnostics.pixelUpdates && diagnostics.pixelCaptureGapsGe45ms <= diagnostics.pixelCaptureGaps,
            "Pixel-capture interval diagnostics exceed the actual updates");
    require(first->cursor.has_value(), "Missing initial cursor snapshot");
    require(first->configuration->desktop_optimization_contract() == 2 && first->frame.has_stage_timing() == optimization, "Support declaration or exact optimization gate mismatch");
    if (optimization) {
        const auto& stages = first->frame.stage_timing();
        require(stages.has_worker_queue_microseconds() && stages.has_acquire_wait_microseconds() && stages.has_source_age_microseconds() == (first->sourcePresentTicks != 0) && stages.has_pacing_credit_wait_microseconds() && stages.has_video_encode_microseconds(), "Actual first frame omitted executed stages");
        require(!stages.has_png_encode_microseconds(), "Video-only path invented PNG duration");
    }
    require(ticks > 0, "Capture/encoding blocked the Ruvia event loop");
    const auto configuration = *first->configuration;
    protocol::DesktopChannelValidator validator({"runtime", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, optimization ? 2u : 0u});
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
    video.feedback(firstDisplayed);

    // No test window change is needed: refresh must also work on a static desktop.
    auto refreshed = co_await video.next(true);
    require(refreshed && refreshed->frame.key_frame() &&
                (refreshed->configuration ? refreshed->generation == first->generation + 1 && refreshed->frameId == 1 : refreshed->generation == first->generation && refreshed->frameId == first->frameId + 1),
            "Refresh did not produce an IDR on the existing or explicitly rebuilt codec baseline");
    require(refreshed->cursor.has_value(), "Refresh did not resend the cursor snapshot");
    require(refreshed->frame.timestamp_microseconds() > first->frame.timestamp_microseconds(), "Frame timestamps are not increasing");
    if (refreshed->configuration) {
        message.set_sequence(message.sequence() + 1);
        message.set_generation(refreshed->generation);
        message.clear_frame_id();
        *message.mutable_video_config() = *refreshed->configuration;
        require(bool(validator.accept(message)), "Refreshed codec configuration violates channel protocol");
    }
    message.set_sequence(message.sequence() + 1);
    message.set_frame_id(refreshed->frameId);
    *message.mutable_video_frame() = refreshed->frame;
    require(bool(validator.accept(message)), "Runtime refresh violates channel protocol");
    decode(*refreshed, refreshed->configuration.value_or(configuration));

    std::optional<agent::DesktopVideoUpdate> rebuilt;
    auto previousTimestamp = refreshed->frame.timestamp_microseconds();
    // Exercise repeated real DXGI/codec teardown rather than only one reset.
    // Each new baseline must work with a fresh independent decoder.
    for (std::uint64_t generation = refreshed->generation + 1; generation <= refreshed->generation + 12; ++generation) {
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
    auto currentGeneration = rebuilt->generation;
    auto currentConfiguration = *rebuilt->configuration;
    bool blocked = false;
    // One GPU-refresh rebuild and one software fallback may reset a generation.
    // They are bounded; repeated refreshes must eventually wait for an ACK.
    for (unsigned i = 0; i < 40; ++i) {
        auto update = co_await video.next(true);
        if (update && !update->frame.data().empty()) {
            if (update->configuration) {
                require(update->generation == currentGeneration + 1 && update->frameId == 1, "Rejected hardware refresh did not rebuild the baseline");
                currentGeneration = update->generation;
                currentConfiguration = *update->configuration;
            }
            lastSent = update->frameId;
        } else {
            require(update && update->cursor, "Congestion stopped cursor refresh");
            blocked = true;
            break;
        }
    }
    require(blocked && lastSent <= 17, "Missing display ACKs did not bound encoded frames");
    feedback.set_displayed_generation(currentGeneration);
    feedback.set_displayed_frame(lastSent);
    video.feedback(feedback);
    auto resumed = co_await video.next(true);
    require(resumed && resumed->frame.key_frame() &&
                (resumed->configuration ? resumed->generation == currentGeneration + 1 && resumed->frameId == 1 : resumed->generation == currentGeneration && resumed->frameId > lastSent),
            "Display ACK did not resume encoding or rebuild a rejected hardware baseline");
    const auto resumedConfiguration = resumed->configuration.value_or(currentConfiguration);
    decode(*resumed, resumedConfiguration);
    feedback.set_displayed_generation(resumed->generation);
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
    require(visible && visible->configuration && visible->frame.key_frame() && visible->frameId == 1 && visible->generation > resumed->generation,
            "Foreground desktop did not rebuild its video baseline");
    decode(*visible, visible->configuration.value_or(resumedConfiguration));

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

    if (changeResolution) {
        std::optional<wire::DisplayMode> alternative;
        std::uint64_t distance = UINT64_MAX;
        for (const auto& mode : configuration.display_modes()) {
            const auto dx = mode.width() > configuration.width() ? mode.width() - configuration.width() : configuration.width() - mode.width();
            const auto dy = mode.height() > configuration.height() ? mode.height() - configuration.height() : configuration.height() - mode.height();
            const auto difference = static_cast<std::uint64_t>(dx) + dy;
            if (difference && difference < distance) {
                alternative = mode;
                distance = difference;
            }
        }
        require(alternative.has_value(), "No alternative display mode for resize coverage");
        auto generation = resized->generation;
        auto timestamp = resized->frame.timestamp_microseconds();
        // Rapid user-requested changes must keep one channel and always publish
        // a fresh, independently decodable baseline, never retained old pixels.
        for (unsigned iteration = 0; iteration < 6; ++iteration) {
            const auto width = iteration % 2 ? configuration.width() : alternative->width();
            const auto height = iteration % 2 ? configuration.height() : alternative->height();
            require(co_await video.resize(width, height), "Advertised display mode was rejected");
            std::optional<agent::DesktopVideoUpdate> changed;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!changed && std::chrono::steady_clock::now() < until) {
                changed = co_await video.next(true);
                if (changed && changed->frame.data().empty()) {
                    changed.reset();
                }
            }
            require(changed && changed->configuration && changed->generation == ++generation && changed->frameId == 1 && changed->frame.key_frame(), "Resize lost the configured key-frame baseline");
            require(changed->configuration->width() == width && changed->configuration->height() == height, "Resize published stale dimensions");
            require(changed->frame.timestamp_microseconds() > timestamp, "Resize reset the presentation clock");
            timestamp = changed->frame.timestamp_microseconds();
            message.set_sequence(message.sequence() + 1);
            message.set_generation(changed->generation);
            message.clear_frame_id();
            *message.mutable_video_config() = *changed->configuration;
            require(bool(validator.accept(message)), "Resize configuration broke the existing channel");
            message.set_sequence(message.sequence() + 1);
            message.set_frame_id(1);
            *message.mutable_video_frame() = changed->frame;
            require(bool(validator.accept(message)), "Resize frame broke the existing channel");
            decode(*changed, *changed->configuration);
            feedback.set_background(false);
            feedback.set_displayed_generation(changed->generation);
            feedback.set_displayed_frame(changed->frameId);
            video.feedback(feedback);
            std::cout << "Resize " << width << 'x' << height << " generation=" << generation << " decoded on original channel\n";
        }
        // Cancel capture immediately after a mode change, before a new baseline.
        // next() may be in capture creation or its recovery timer when stopped.
        require(co_await video.resize(alternative->width(), alternative->height()), "Pre-cancellation resize failed");
    }

    // Stop while next() is suspended, then await its completion before teardown.
    pulse.cancel();
    const auto stopRequested = std::make_shared<bool>(false);
    // A previously queued successful pulse callback can rearm pulse after
    // cancel(). Keep the stop timer independent of that recurring test clock.
    asio::steady_timer stopTimer(pulse.get_executor());
    stopTimer.expires_after(std::chrono::milliseconds(1));
    stopTimer.async_wait([&video, stopRequested](std::error_code error) {
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
    require(std::chrono::steady_clock::now() < stopDeadline, "Capture cancellation exceeded its completion deadline");
    require(!(co_await video.next(true)), "Stopped video runtime restarted capture");
}
} // namespace
int main(int argc, char** argv) {
    try {
        bool changeResolution = false, optimization = false;
        for (int index = 1; index < argc; ++index) {
            const auto option = std::string_view(argv[index]);
            if (option == "--resize") {
                changeResolution = true;
            } else if (option == "--optimization=v2") {
                optimization = true;
            } else {
                throw std::invalid_argument("Usage: agent-video-runtime-tests [--resize] [--optimization=v2]");
            }
        }
        DisplayRestore displayRestore(changeResolution);
        require(SetThreadExecutionState(ES_DISPLAY_REQUIRED) != 0, "Cannot wake test display");
        asio::io_context io;
        const auto coreCount = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
        ruvia::blocking_pool blockingPool{{.thread_count_ = coreCount, .queue_capacity_ = std::max<std::size_t>(64u, coreCount * 16u)}};
        ruvia::event_loop_attachment attachment(io, {.blocking_pool_ = &blockingPool});
        agent::DesktopVideoRuntime video(attachment.loop(), blockingPool, 0, optimization ? 2 : 0);
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
        asio::co_spawn(attachment.loop().executor(), verify(video, pulse, ticks, changeResolution, optimization), [&](std::exception_ptr error) {
            failure = error;
            pulse.cancel();
            video.stop();
            attachment.request_stop();
        });
        attachment.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        displayRestore.restore();
        std::cout << "Real desktop capture/encode, twelve baseline rebuilds, independent refresh decode, event-loop responsiveness and stop passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
