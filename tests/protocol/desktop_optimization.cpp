#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include "ultraterimal/protocol/desktop_ipc.protocol.h"

namespace {
using namespace ultraterimal;
using namespace ultraterimal::protocol;
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
wire::Envelope envelope(std::uint64_t sequence, const char* channel = "video") {
    wire::Envelope value;
    value.set_protocol_version(kVersion);
    value.set_session_id("s");
    value.set_channel_id(channel);
    value.set_sequence(sequence);
    return value;
}
wire::Envelope config(std::uint64_t sequence = 2, std::uint64_t generation = 1, std::uint32_t contract = 2, std::uint32_t width = 256, std::uint32_t height = 128) {
    auto value = envelope(sequence);
    value.set_generation(generation);
    auto* body = value.mutable_video_config();
    body->set_codec("avc1.42E01F");
    body->set_width(width);
    body->set_height(height);
    body->set_desktop_optimization_contract(contract);
    body->set_continuous_frames(contract == 2);
    return value;
}
wire::Envelope frame(std::uint64_t sequence = 3, std::uint64_t generation = 1, std::uint64_t id = 1) {
    auto value = envelope(sequence);
    value.set_generation(generation);
    value.set_frame_id(id);
    value.mutable_video_frame()->set_key_frame(true);
    value.mutable_video_frame()->set_data("key");
    value.mutable_video_frame()->set_timestamp_microseconds(id * 1000);
    return value;
}
wire::Envelope repeatedFrame(std::uint64_t sequence = 4, std::uint64_t generation = 1, std::uint64_t id = 2, std::uint64_t base = 1) {
    auto value = frame(sequence, generation, id);
    value.set_base_frame_id(base);
    value.mutable_video_frame()->set_key_frame(false);
    value.mutable_video_frame()->set_repeated(true);
    return value;
}
DesktopChannelValidator video(std::uint32_t contract = 2) {
    DesktopChannelValidator validator({"s", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, false, contract});
    validator.setContinuousFrames(true);
    return validator;
}
void start(DesktopChannelValidator& validator, const wire::Envelope& configuration = config()) {
    require(validator.accept(configuration).has_value(), "Config rejected");
    require(validator.accept(frame(3, configuration.generation())).has_value(), "Key frame rejected");
}
void verifyGate() {
    for (const auto permission : {0u, 1u, 2u, 3u, std::numeric_limits<std::uint32_t>::max()}) {
        for (const auto advertised : {0u, 1u, 2u, 3u, std::numeric_limits<std::uint32_t>::max()}) {
            auto validator = video(permission);
            start(validator, config(2, 1, advertised));
            const bool active = permission == 2 && advertised == 2;
            require(validator.accept(repeatedFrame()).has_value() == active, "Only exact two-sided v2 gate may activate continuous frames");
        }
    }
    DesktopChannelValidator defaultBinding({"s", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, false});
    defaultBinding.setContinuousFrames(true);
    start(defaultBinding);
    require(!defaultBinding.accept(repeatedFrame()), "Default binding activated v2");
    defaultBinding.setOptimizationContract(2);
    require(!defaultBinding.accept(repeatedFrame()), "Setter activated current generation");
    auto badConfig = config(4, 2);
    badConfig.set_frame_id(1);
    require(!defaultBinding.accept(badConfig), "Malformed barrier accepted");
    require(!defaultBinding.accept(repeatedFrame()), "Failed barrier activated v2");
    require(defaultBinding.accept(config(4, 2)).has_value(), "New barrier rejected");
    require(defaultBinding.accept(frame(5, 2)).has_value(), "New key rejected");
    require(defaultBinding.accept(repeatedFrame(6, 2, 2, 1)).has_value(), "Setter did not activate next generation");
    defaultBinding.setOptimizationContract(99);
    require(defaultBinding.accept(repeatedFrame(7, 2, 3, 1)).has_value(), "Setter disabled active generation without barrier");
    require(defaultBinding.accept(config(8, 3)).has_value(), "Disabled barrier rejected");
    require(defaultBinding.accept(frame(9, 3)).has_value(), "Disabled key rejected");
    require(!defaultBinding.accept(repeatedFrame(10, 3, 2, 1)), "Disabled permission persisted into next generation");
}
// Reflection tests every duration field and its independent optional presence.
void checkDurationFields(wire::Envelope& value, google::protobuf::Message& timing, int fields, int first = 1) {
    for (int index = first; index <= fields; ++index) {
        const auto* field = timing.GetDescriptor()->FindFieldByNumber(index);
        if (!field) {
            continue;
        }
        const auto* reflection = timing.GetReflection();
        require(field->has_presence(), "Duration lacks presence");
        reflection->ClearField(&timing, field);
        require(!reflection->HasField(timing, field), "Absent duration reported present");
        for (auto duration : {0u, 60000000u, 60000001u, std::numeric_limits<std::uint32_t>::max()}) {
            reflection->SetUInt32(&timing, field, duration);
            require(validateMessage(value).has_value() == (duration <= 60000000), "Duration hard bound failed");
        }
        reflection->SetUInt32(&timing, field, 0);
        require(reflection->HasField(timing, field), "Measured zero lost presence");
        const auto encoded = encodeMessage(value);
        require(encoded.has_value(), "Zero presence encode failed");
        const auto decoded = decodeMessage(*encoded);
        require(decoded && decoded->SerializeAsString() == value.SerializeAsString(), "Presence lost in wire roundtrip");
        reflection->ClearField(&timing, field);
        const auto absent = encodeMessage(value);
        require(absent && decodeMessage(*absent)->SerializeAsString() == value.SerializeAsString(), "Absent duration changed on wire");
    }
}
wire::Envelope report(std::uint64_t sequence = 2) {
    auto value = envelope(sequence, "input");
    auto* sample = value.mutable_desktop_pipeline_report()->add_samples();
    sample->set_generation(1);
    sample->set_frame_id(1);
    sample->set_source(wire::DesktopPipelineSample::DESKTOP_WORKER);
    return value;
}
void verifyTimingAndMalformed() {
    auto value = frame();
    checkDurationFields(value, *value.mutable_video_frame()->mutable_stage_timing(), 9);
    auto feedback = envelope(2, "input");
    feedback.mutable_quality_feedback()->set_displayed_generation(1);
    feedback.mutable_quality_feedback()->set_displayed_frame(1);
    checkDurationFields(feedback, *feedback.mutable_quality_feedback()->mutable_browser_timing(), 5);
    feedback.mutable_quality_feedback()->set_displayed_frame(0);
    require(!validateMessage(feedback), "Unassociated browser duration accepted");
    feedback.mutable_quality_feedback()->set_displayed_frame(1);
    feedback.mutable_quality_feedback()->set_displayed_generation(0);
    require(!validateMessage(feedback), "Unassociated browser generation accepted");
    feedback.mutable_quality_feedback()->clear_browser_timing();
    require(validateMessage(feedback).has_value(), "Plain feedback rejected");
    auto diagnostic = report();
    checkDurationFields(diagnostic, *diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0), 7, 5);
    auto* sample = diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0);
    sample->set_source(static_cast<wire::DesktopPipelineSample::Source>(99));
    require(!validateMessage(diagnostic), "Unknown diagnostic source accepted");
    sample->set_source(wire::DesktopPipelineSample::UNSPECIFIED);
    require(!validateMessage(diagnostic), "Unspecified diagnostic source accepted");
    sample->set_source(wire::DesktopPipelineSample::AGENT_RELAY);
    sample->set_batch_id(std::numeric_limits<std::uint64_t>::max());
    require(validateMessage(diagnostic).has_value(), "Relay sample rejected");
    const auto valid = *sample;
    for (int index = 1; index < 32; ++index) {
        *diagnostic.mutable_desktop_pipeline_report()->add_samples() = valid;
    }
    require(validateMessage(diagnostic).has_value(), "32 diagnostic samples rejected");
    *diagnostic.mutable_desktop_pipeline_report()->add_samples() = valid;
    require(!validateMessage(diagnostic), "33 diagnostic samples accepted");
    diagnostic = report();
    diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0)->set_frame_id(0);
    require(!validateMessage(diagnostic), "Zero diagnostic frame accepted");
    diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0)->set_frame_id(1);
    diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0)->set_generation(0);
    require(!validateMessage(diagnostic), "Zero diagnostic generation accepted");
    diagnostic.mutable_desktop_pipeline_report()->clear_samples();
    require(!validateMessage(diagnostic), "Empty diagnostic report accepted");
    diagnostic.mutable_desktop_pipeline_report()->set_dropped_samples(1);
    require(validateMessage(diagnostic).has_value(), "Dropped-only report rejected");
    diagnostic.set_generation(1);
    require(!validateMessage(diagnostic), "Report envelope carried frame metadata");
}
void verifyDirectionsAndIpc() {
    for (const auto source : {ChannelSource::kBrowser, ChannelSource::kAgent}) {
        for (const auto purpose : {wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO}) {
            DesktopChannelValidator validator({"s", purpose == wire::DESKTOP_INPUT ? "input" : "video", purpose, source, false});
            auto value = report();
            value.set_channel_id(purpose == wire::DESKTOP_INPUT ? "input" : "video");
            const bool allowed = purpose == wire::DESKTOP_INPUT && source == ChannelSource::kAgent;
            const auto result = validator.accept(value);
            require(result.has_value() == allowed, "Diagnostic direction whitelist failed");
            if (!allowed) {
                require(result.error() == DesktopMessageError::kDirection, "Wrong direction error");
            }
        }
    }
    for (const auto contract : {0u, 2u}) {
        DesktopPipeValidator desktop({"s", 7, false, DesktopPipeSource::kDesktop, contract});
        desktop.setContinuousFrames(true);
        auto ready = envelope(1, "");
        ready.mutable_desktop_ready();
        require(desktop.accept(ready).has_value(), "IPC ready failed");
        require(desktop.accept(config()).has_value(), "IPC config failed");
        require(desktop.accept(frame()).has_value(), "IPC key failed");
        require(desktop.accept(repeatedFrame()).has_value() == (contract == 2), "IPC binding gate did not propagate");
        desktop.setOptimizationContract(2);
        require(desktop.accept(config(5, 2)).has_value(), "IPC barrier failed");
        require(desktop.accept(frame(6, 2)).has_value(), "IPC new key failed");
        require(desktop.accept(repeatedFrame(7, 2, 2, 1)).has_value(), "IPC setter did not propagate");
        require(desktop.accept(report(8)).has_value(), "IPC worker diagnostic rejected");
        DesktopPipeValidator service({"s", 7, false, DesktopPipeSource::kService});
        auto initial = envelope(1, "");
        initial.mutable_desktop_start()->set_windows_session_id(7);
        initial.mutable_desktop_start()->set_desktop_optimization_contract(contract);
        const auto encoded = encodeMessage(initial);
        require(encoded && decodeMessage(*encoded)->desktop_start().desktop_optimization_contract() == contract, "DesktopStart gate lost on wire");
        require(service.accept(initial).has_value(), "IPC service start failed");
        auto misplaced = report(9);
        misplaced.set_channel_id("video");
        require(!desktop.accept(misplaced), "IPC video diagnostic accepted");
        require(!service.accept(report(3)), "IPC service diagnostic accepted");
    }
    for (const auto contract : {0u, 2u, 99u}) {
        auto ready = envelope(1, "input");
        ready.mutable_channel_ready()->set_purpose(wire::DESKTOP_INPUT);
        ready.mutable_channel_ready()->set_desktop_optimization_contract(contract);
        const auto encoded = encodeMessage(ready);
        require(encoded && decodeMessage(*encoded)->channel_ready().desktop_optimization_contract() == contract, "ChannelReady gate compatibility lost");
        auto capability = envelope(2, "input");
        capability.mutable_desktop_video_capabilities()->set_desktop_optimization_contract(contract);
        require(validateMessage(capability).has_value(), "Unknown capability version must stay non-activating, not malformed");
    }
}
} // namespace
int main() {
    try {
        verifyGate();
        verifyTimingAndMalformed();
        verifyDirectionsAndIpc();
        std::cout << "Desktop v2 gate, timing presence, diagnostic directions and IPC passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
