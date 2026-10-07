#include <iostream>
#include <stdexcept>

#include "ultraterimal/protocol/desktop.protocol.h"

namespace {
using namespace ultraterimal;
using namespace ultraterimal::protocol;
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Desktop channel invariant failed");
    }
}
bool rejected(const std::expected<void, DesktopMessageError>& result, DesktopMessageError error) { return !result && result.error() == error; }
wire::Envelope envelope(std::uint64_t sequence, std::string channel = "video") {
    wire::Envelope message;
    message.set_protocol_version(kVersion);
    message.set_session_id("session");
    message.set_channel_id(channel);
    message.set_sequence(sequence);
    return message;
}
void verifyPermissions() {
    DesktopChannelValidator viewer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    auto input = envelope(2, "input");
    input.set_generation(1);
    input.mutable_input_event()->set_kind(wire::InputEvent::KEY);
    input.mutable_input_event()->set_code(65);
    require(rejected(viewer.accept(input), DesktopMessageError::kReadOnly));
    DesktopChannelValidator writer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, true});
    auto missingGeneration = input;
    missingGeneration.set_generation(0);
    require(rejected(writer.accept(missingGeneration), DesktopMessageError::kMalformed));
    auto framedInput = input;
    framedInput.set_frame_id(1);
    require(rejected(writer.accept(framedInput), DesktopMessageError::kMalformed));
    auto wrong = input;
    wrong.set_session_id("other");
    require(rejected(writer.accept(wrong), DesktopMessageError::kBinding));
    wrong = input;
    wrong.set_channel_id("video");
    require(rejected(writer.accept(wrong), DesktopMessageError::kBinding));
    require(writer.accept(input).has_value());
    require(rejected(writer.accept(input), DesktopMessageError::kReplay));
    auto feedback = envelope(2, "input");
    feedback.mutable_quality_feedback()->set_queue_depth(1);
    require(viewer.accept(feedback).has_value());
    feedback.set_sequence(3);
    feedback.mutable_quality_feedback()->set_preference(static_cast<wire::QualityFeedback::Preference>(99));
    require(rejected(viewer.accept(feedback), DesktopMessageError::kMalformed));
    feedback.set_sequence(2);
    feedback.mutable_quality_feedback()->set_preference(wire::QualityFeedback::SHARP);
    DesktopChannelValidator qualityViewer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    require(qualityViewer.accept(feedback).has_value());
    auto timed = envelope(2, "input");
    timed.mutable_quality_feedback()->set_displayed_frame(1);
    auto* timing = timed.mutable_quality_feedback()->mutable_visible_frame_timing();
    timing->set_sample_count(120);
    timing->set_p95_microseconds(48000);
    timing->set_p99_microseconds(64000);
    timing->set_long_gap_count(3);
    timing->set_observed_fps_milli(27500);
    const auto encodedTiming = encodeMessage(timed);
    require(encodedTiming.has_value());
    const auto decodedTiming = decodeMessage(*encodedTiming);
    require(decodedTiming && decodedTiming->quality_feedback().visible_frame_timing().p95_microseconds() == 48000);
    DesktopChannelValidator timedViewer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    require(timedViewer.accept(*decodedTiming).has_value());
    const auto invalidTiming = [&](const wire::Envelope& message) {
        DesktopChannelValidator validator({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
        return rejected(validator.accept(message), DesktopMessageError::kMalformed);
    };
    timing->set_sample_count(121);
    require(invalidTiming(timed));
    timing->set_sample_count(19);
    require(invalidTiming(timed));
    timing->set_sample_count(120);
    timing->set_p99_microseconds(47000);
    require(invalidTiming(timed));
    timing->set_p99_microseconds(64000);
    timing->set_long_gap_count(121);
    require(invalidTiming(timed));
    timing->set_long_gap_count(3);
    timing->set_p95_microseconds(0);
    require(invalidTiming(timed));
    timing->set_p95_microseconds(30000001);
    timing->set_p99_microseconds(30000001);
    require(invalidTiming(timed));
    timing->set_p95_microseconds(48000);
    timing->set_p99_microseconds(30000001);
    require(invalidTiming(timed));
    timing->set_p99_microseconds(64000);
    timing->set_observed_fps_milli(0);
    require(invalidTiming(timed));
    timing->set_observed_fps_milli(240001);
    require(invalidTiming(timed));
    timing->set_observed_fps_milli(27500);
    timed.mutable_quality_feedback()->set_background(true);
    require(invalidTiming(timed));
    for (const auto capacity : {0u, 128u}) {
        auto capable = envelope(2, "input");
        capable.mutable_quality_feedback()->set_maximum_pending_video_frames(capacity);
        DesktopChannelValidator validator({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
        require(validator.accept(capable).has_value());
    }
    auto unsupportedCapacity = envelope(2, "input");
    unsupportedCapacity.mutable_quality_feedback()->set_maximum_pending_video_frames(129);
    DesktopChannelValidator unsupportedValidator({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    require(rejected(unsupportedValidator.accept(unsupportedCapacity), DesktopMessageError::kMalformed));
    auto refresh = envelope(3, "input");
    refresh.mutable_request_refresh();
    require(viewer.accept(refresh).has_value());
    auto resize = envelope(4, "input");
    resize.mutable_desktop_resize()->set_request_id(1);
    resize.mutable_desktop_resize()->set_width(1920);
    resize.mutable_desktop_resize()->set_height(1080);
    require(rejected(viewer.accept(resize), DesktopMessageError::kReadOnly));
    require(writer.accept(resize).has_value());
    resize.set_sequence(5);
    resize.mutable_desktop_resize()->set_width(1919);
    require(rejected(writer.accept(resize), DesktopMessageError::kMalformed));
    resize.mutable_desktop_resize()->set_width(4098);
    require(rejected(writer.accept(resize), DesktopMessageError::kMalformed));
    resize.mutable_desktop_resize()->set_width(1920);
    resize.mutable_desktop_resize()->set_request_id(0);
    require(rejected(writer.accept(resize), DesktopMessageError::kMalformed));
    DesktopChannelValidator agent({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kAgent, true});
    require(rejected(agent.accept(input), DesktopMessageError::kDirection));
    auto cursor = envelope(2, "input");
    cursor.mutable_cursor_update()->set_visible(false);
    require(agent.accept(cursor).has_value());
    cursor.set_sequence(3);
    cursor.mutable_cursor_update()->set_width(2);
    cursor.mutable_cursor_update()->set_height(2);
    cursor.mutable_cursor_update()->set_rgba(std::string(15, '\0'));
    require(rejected(agent.accept(cursor), DesktopMessageError::kMalformed));
    cursor.mutable_cursor_update()->set_rgba(std::string(16, '\0'));
    cursor.mutable_cursor_update()->set_hotspot_x(2);
    require(rejected(agent.accept(cursor), DesktopMessageError::kMalformed));
    cursor.mutable_cursor_update()->set_hotspot_x(1);
    require(agent.accept(cursor).has_value());
    cursor.set_sequence(4);
    require(rejected(viewer.accept(cursor), DesktopMessageError::kDirection));
}
void verifySecureAttention() {
    auto request = envelope(2, "input");
    request.set_generation(1);
    request.mutable_input_event()->set_kind(wire::InputEvent::SECURE_ATTENTION);
    DesktopChannelValidator viewer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    DesktopChannelValidator writer({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, true});
    DesktopChannelValidator agent({"session", "input", wire::DESKTOP_INPUT, ChannelSource::kAgent, true});
    require(rejected(viewer.accept(request), DesktopMessageError::kReadOnly));
    auto malformed = request;
    malformed.mutable_input_event()->set_code(46);
    require(rejected(writer.accept(malformed), DesktopMessageError::kMalformed));
    malformed = request;
    malformed.set_generation(0);
    require(rejected(writer.accept(malformed), DesktopMessageError::kMalformed));
    require(rejected(agent.accept(request), DesktopMessageError::kDirection));
    const auto encoded = encodeMessage(request);
    require(encoded.has_value());
    require(writer.accept(*decodeMessage(*encoded)).has_value());
    require(rejected(writer.accept(request), DesktopMessageError::kReplay));
    auto result = envelope(3, "input");
    result.mutable_desktop_secure_attention_result()->set_win32_error(1260);
    require(agent.accept(result).has_value());
    require(rejected(writer.accept(result), DesktopMessageError::kDirection));
}
void verifyProbe() {
    for (const auto source : {ChannelSource::kAgent, ChannelSource::kBrowser}) {
        DesktopChannelValidator input({"session", "input", wire::DESKTOP_INPUT, source, false});
        auto probe = envelope(2, "input");
        probe.mutable_desktop_probe()->set_nonce(1);
        require(input.accept(probe).has_value());
        require(rejected(input.accept(probe), DesktopMessageError::kReplay));
        probe.set_sequence(3);
        probe.mutable_desktop_probe()->set_nonce(0);
        require(rejected(input.accept(probe), DesktopMessageError::kMalformed));
        probe.mutable_desktop_probe()->set_nonce(2);
        probe.set_generation(1);
        require(rejected(input.accept(probe), DesktopMessageError::kMalformed));
        probe.set_generation(0);
        probe.mutable_desktop_probe()->set_reset_input(true);
        probe.mutable_desktop_probe()->set_accepted_response_nonce(2);
        require(rejected(input.accept(probe), DesktopMessageError::kMalformed));
        probe.mutable_desktop_probe()->set_accepted_response_nonce(1);
        wire::Envelope decoded;
        require(decoded.ParseFromString(probe.SerializeAsString()) && decoded.desktop_probe().reset_input());
        if (source == ChannelSource::kAgent) {
            require(input.accept(decoded).has_value());
        } else {
            require(rejected(input.accept(decoded), DesktopMessageError::kDirection));
            decoded.mutable_desktop_probe()->set_reset_input(false);
            require(rejected(input.accept(decoded), DesktopMessageError::kDirection));
            decoded.mutable_desktop_probe()->set_accepted_response_nonce(0);
            require(input.accept(decoded).has_value());
        }
        DesktopChannelValidator video({"session", "video", wire::DESKTOP_VIDEO, source, false});
        probe.set_channel_id("video");
        require(rejected(video.accept(probe), DesktopMessageError::kDirection));
    }
}
void verifyFrames() {
    DesktopChannelValidator video({"session", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, true});
    auto config = envelope(2);
    config.set_generation(1);
    config.mutable_video_config()->set_width(16);
    config.mutable_video_config()->set_height(16);
    config.mutable_video_config()->set_codec("avc1.42C01E");
    auto frame = envelope(3);
    frame.set_generation(1);
    frame.set_frame_id(1);
    frame.mutable_video_frame()->set_data("payload");
    require(!frame.video_frame().has_target_frames_per_second() && !frame.video_frame().has_target_bitrate()); // Legacy Agent.
    require(rejected(video.accept(frame), DesktopMessageError::kFrameState));
    require(video.accept(config).has_value());
    require(rejected(video.accept(frame), DesktopMessageError::kFrameState));
    frame.mutable_video_frame()->set_key_frame(true);
    require(video.accept(frame).has_value());
    frame.set_sequence(4);
    frame.set_frame_id(2);
    require(rejected(video.accept(frame), DesktopMessageError::kFrameState));
    frame.mutable_video_frame()->set_timestamp_microseconds(33333);
    frame.mutable_video_frame()->set_target_frames_per_second(60);
    frame.mutable_video_frame()->set_target_bitrate(8000000);
    require(video.accept(frame).has_value());
    auto malformedRate = frame;
    malformedRate.set_sequence(5);
    malformedRate.set_frame_id(3);
    malformedRate.mutable_video_frame()->set_target_bitrate(63000);
    require(rejected(video.accept(malformedRate), DesktopMessageError::kMalformed));
    malformedRate = frame;
    malformedRate.set_sequence(5);
    malformedRate.mutable_video_frame()->clear_target_bitrate();
    require(rejected(video.accept(malformedRate), DesktopMessageError::kMalformed));
    auto full = envelope(5);
    full.set_generation(1);
    full.set_frame_id(3);
    full.mutable_full_frame()->set_width(16);
    full.mutable_full_frame()->set_height(16);
    full.mutable_full_frame()->set_rgba(std::string(16 * 16 * 4, '\0'));
    require(rejected(video.accept(full), DesktopMessageError::kFrameState));
    full.set_generation(2);
    full.set_frame_id(1);
    require(video.accept(full).has_value());
    auto diff = envelope(6);
    diff.set_generation(2);
    diff.set_base_frame_id(1);
    diff.set_frame_id(2);
    auto* copy = diff.mutable_diff_batch()->add_copies();
    copy->mutable_destination()->set_width(8);
    copy->mutable_destination()->set_height(8);
    copy->set_source_x(9);
    require(rejected(video.accept(diff), DesktopMessageError::kFrameState));
    copy->set_source_x(8);
    require(video.accept(diff).has_value());
    diff.set_sequence(7);
    diff.set_frame_id(3);
    require(rejected(video.accept(diff), DesktopMessageError::kFrameState));
    diff.set_base_frame_id(2);
    require(video.accept(diff).has_value());
    config.set_sequence(8);
    config.set_generation(2);
    require(rejected(video.accept(config), DesktopMessageError::kFrameState));
    config.set_generation(3);
    require(video.accept(config).has_value());
    DesktopChannelValidator browser({"session", "video", wire::DESKTOP_VIDEO, ChannelSource::kBrowser, true});
    require(rejected(browser.accept(config), DesktopMessageError::kDirection));
}
void verifyRefinement() {
    DesktopChannelValidator video({"session", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, true});
    auto config = envelope(2);
    config.set_generation(1);
    config.mutable_video_config()->set_width(16);
    config.mutable_video_config()->set_height(16);
    config.mutable_video_config()->set_codec("avc1.42C01E");
    require(video.accept(config).has_value());
    auto frame = envelope(3);
    frame.set_generation(1);
    frame.set_frame_id(1);
    frame.mutable_video_frame()->set_key_frame(true);
    frame.mutable_video_frame()->set_data("frame");
    require(video.accept(frame).has_value());
    auto detail = envelope(4);
    detail.set_generation(1);
    detail.set_frame_id(1);
    detail.set_base_frame_id(1);
    auto* patch = detail.mutable_video_refinement()->add_patches();
    patch->mutable_destination()->set_width(8);
    patch->mutable_destination()->set_height(8);
    std::string png("\211PNG\r\n\032\n", 8);
    png.resize(33);
    png[11] = 13;
    png.replace(12, 4, "IHDR");
    png[19] = png[23] = 8;
    patch->set_png(png); // Header validation; actual PNG decoding is tested by agent.refinement.
    auto bad = detail;
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_png()->at(11) = 12;
    require(rejected(video.accept(bad), DesktopMessageError::kMalformed));
    bad = detail;
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_png()->at(19) = 9;
    require(rejected(video.accept(bad), DesktopMessageError::kMalformed));
    bad = detail;
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_x(9);
    require(rejected(video.accept(bad), DesktopMessageError::kFrameState));
    bad = detail;
    bad.set_generation(2);
    require(rejected(video.accept(bad), DesktopMessageError::kFrameState));
    bad = detail;
    bad.set_frame_id(2);
    bad.set_base_frame_id(2);
    require(rejected(video.accept(bad), DesktopMessageError::kFrameState));
    require(video.accept(detail).has_value());
    detail.set_sequence(5);
    require(rejected(video.accept(detail), DesktopMessageError::kFrameState));
    frame.set_sequence(5);
    frame.set_frame_id(2);
    frame.mutable_video_frame()->set_timestamp_microseconds(16667);
    require(video.accept(frame).has_value());
    detail.set_sequence(6);
    require(rejected(video.accept(detail), DesktopMessageError::kFrameState));
    detail.set_frame_id(2);
    detail.set_base_frame_id(2);
    require(video.accept(detail).has_value());
}
void verifyContinuousFrames() {
    const auto configure = [](DesktopChannelValidator& validator, bool continuous) {
        auto config = envelope(2);
        config.set_generation(1);
        config.mutable_video_config()->set_width(128);
        config.mutable_video_config()->set_height(64);
        config.mutable_video_config()->set_codec("avc1.42C01E");
        config.mutable_video_config()->set_desktop_optimization_contract(2);
        config.mutable_video_config()->set_continuous_frames(continuous);
        require(validator.accept(config).has_value());
        auto frame = envelope(3);
        frame.set_generation(1);
        frame.set_frame_id(1);
        frame.mutable_video_frame()->set_key_frame(true);
        frame.mutable_video_frame()->set_data("frame");
        require(validator.accept(frame).has_value());
        return frame;
    };
    DesktopChannelValidator video({"session", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, true, 2});
    video.setContinuousFrames(true);
    auto repeat = configure(video, true);
    repeat.set_sequence(4);
    repeat.set_frame_id(2);
    repeat.set_base_frame_id(1);
    repeat.mutable_video_frame()->set_repeated(true);
    repeat.mutable_video_frame()->set_timestamp_microseconds(16667);
    require(video.accept(repeat).has_value());
    repeat.set_sequence(5);
    repeat.set_frame_id(3);
    repeat.set_base_frame_id(2);
    repeat.mutable_video_frame()->set_timestamp_microseconds(33333);
    require(rejected(video.accept(repeat), DesktopMessageError::kFrameState));
    repeat.set_base_frame_id(1);
    require(video.accept(repeat).has_value());
    auto detail = envelope(6);
    detail.set_generation(1);
    detail.set_frame_id(1);
    detail.set_base_frame_id(1);
    detail.mutable_video_refinement()->set_batch_id(1);
    detail.mutable_video_refinement()->set_kind(wire::STATIC_DETAIL);
    auto* patch = detail.mutable_video_refinement()->add_patches();
    patch->mutable_destination()->set_width(128);
    patch->mutable_destination()->set_height(64);
    std::string png("\211PNG\r\n\032\n", 8);
    png.resize(33);
    png[11] = 13;
    png.replace(12, 4, "IHDR");
    png[19] = static_cast<char>(128);
    png[23] = 64;
    patch->set_png(png);
    require(video.accept(detail).has_value());
    detail.set_sequence(7);
    detail.mutable_video_refinement()->set_batch_id(2);
    require(rejected(video.accept(detail), DesktopMessageError::kFrameState)); // Repeat did not erase the refined grid.
    repeat.set_sequence(7);
    repeat.set_frame_id(4);
    repeat.set_base_frame_id(0);
    repeat.mutable_video_frame()->set_repeated(false);
    repeat.mutable_video_frame()->set_timestamp_microseconds(50000);
    require(video.accept(repeat).has_value());
    detail.set_sequence(8);
    require(rejected(video.accept(detail), DesktopMessageError::kFrameState));
    detail.set_frame_id(4);
    detail.set_base_frame_id(4);
    require(video.accept(detail).has_value());
    DesktopChannelValidator legacy({"session", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, true, 2});
    repeat = configure(legacy, true);
    repeat.set_sequence(4);
    repeat.set_frame_id(2);
    repeat.set_base_frame_id(1);
    repeat.mutable_video_frame()->set_repeated(true);
    repeat.mutable_video_frame()->set_timestamp_microseconds(16667);
    require(rejected(legacy.accept(repeat), DesktopMessageError::kFrameState));
}
} // namespace
int main() {
    try {
        verifyPermissions();
        verifySecureAttention();
        verifyProbe();
        verifyFrames();
        verifyRefinement();
        verifyContinuousFrames();
        std::cout << "Desktop channel binding, direction, read-only access, replay and frame continuity passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
