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
void patch(wire::VideoRefinement& body, std::uint32_t x = 0, std::uint32_t y = 0, std::uint32_t width = 128, std::uint32_t height = 64, std::size_t bytes = 33) {
    auto* item = body.add_patches();
    auto* area = item->mutable_destination();
    area->set_x(x);
    area->set_y(y);
    area->set_width(width);
    area->set_height(height);
    std::string png(bytes, '\0');
    png.replace(0, 8, "\211PNG\r\n\032\n", 8);
    png[11] = 13;
    png.replace(12, 4, "IHDR");
    png[19] = static_cast<char>(width);
    png[23] = static_cast<char>(height);
    item->set_png(png); // Header-only fixture, not a PNG decoder test.
}
wire::Envelope detail(std::uint64_t sequence = 4, std::uint64_t batch = 1, std::uint32_t x = 0, std::uint32_t y = 0) {
    auto value = envelope(sequence);
    value.set_generation(1);
    value.set_frame_id(1);
    value.set_base_frame_id(1);
    auto* body = value.mutable_video_refinement();
    body->set_batch_id(batch);
    body->set_kind(batch ? wire::STATIC_DETAIL : wire::UNSPECIFIED);
    patch(*body, x, y);
    return value;
}
DesktopChannelValidator video(std::uint32_t contract = 2) {
    return DesktopChannelValidator({"s", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, false, contract});
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
            require(validator.accept(detail()).has_value() == active, "Only exact two-sided v2 gate may activate");
            require(validator.accept(detail(active ? 5 : 4, 0)).has_value() != active, "Legacy path must remain separate");
        }
    }
    DesktopChannelValidator defaultBinding({"s", "video", wire::DESKTOP_VIDEO, ChannelSource::kAgent, false});
    start(defaultBinding);
    require(!defaultBinding.accept(detail()), "Default binding activated v2");
    defaultBinding.setOptimizationContract(2);
    require(!defaultBinding.accept(detail()), "Setter activated current generation");
    auto badConfig = config(4, 2);
    badConfig.set_frame_id(1);
    require(!defaultBinding.accept(badConfig), "Malformed barrier accepted");
    require(!defaultBinding.accept(detail()), "Failed barrier activated v2");
    require(defaultBinding.accept(config(4, 2)).has_value(), "New barrier rejected");
    require(defaultBinding.accept(frame(5, 2)).has_value(), "New key rejected");
    auto next = detail(6);
    next.set_generation(2);
    require(defaultBinding.accept(next).has_value(), "Setter did not activate next generation");
    defaultBinding.setOptimizationContract(99);
    next = detail(7, 7, 128);
    next.set_generation(2);
    require(defaultBinding.accept(next).has_value(), "Setter disabled active generation without barrier");
    require(defaultBinding.accept(config(8, 3)).has_value(), "Disabled barrier rejected");
    require(defaultBinding.accept(frame(9, 3)).has_value(), "Disabled key rejected");
    next = detail(10, 8);
    next.set_generation(3);
    require(!defaultBinding.accept(next), "Disabled permission persisted into next generation");
    next.mutable_video_refinement()->set_batch_id(0);
    next.mutable_video_refinement()->set_kind(wire::UNSPECIFIED);
    require(defaultBinding.accept(next).has_value(), "Legacy after rollback rejected");
    next.set_sequence(11);
    require(!defaultBinding.accept(next), "Legacy single-batch guard lost");
    auto legacyCache = video(0);
    start(legacyCache, config(2, 1, 0));
    next = detail(4, 0);
    next.mutable_video_refinement()->mutable_patches(0)->set_cache_slot(1);
    require(legacyCache.accept(next).has_value(), "Legacy cache definition lost");
    require(legacyCache.accept(frame(5, 1, 2)).has_value(), "Legacy new frame failed");
    next.set_sequence(6);
    next.set_frame_id(2);
    next.set_base_frame_id(2);
    next.mutable_video_refinement()->mutable_patches(0)->clear_png();
    require(legacyCache.accept(next).has_value(), "Legacy cache reference lost");
}
void verifyBatches() {
    auto validator = video();
    start(validator);
    auto bad = detail();
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_x(1);
    require(!validator.accept(bad), "Off-grid tile accepted");
    bad = detail();
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_width(64);
    bad.mutable_video_refinement()->mutable_patches(0)->mutable_png()->at(19) = 64;
    require(!validator.accept(bad), "Interior partial tile accepted");
    bad = detail();
    patch(*bad.mutable_video_refinement(), 0, 0);
    require(!validator.accept(bad), "Duplicate within batch accepted");
    bad = detail();
    patch(*bad.mutable_video_refinement(), 256, 0);
    require(!validator.accept(bad), "Out-of-canvas batch accepted");
    bad = detail();
    bad.mutable_video_refinement()->mutable_patches(0)->set_cache_slot(1);
    require(!validator.accept(bad), "v2 cache definition accepted");
    bad.mutable_video_refinement()->mutable_patches(0)->clear_png();
    require(!validator.accept(bad), "v2 cache reference accepted");
    require(validator.accept(detail()).has_value(), "Failure mutated sequence/grid/batch state");
    require(!validator.accept(detail(5, 2)), "Repeated tile across batches accepted");
    require(!validator.accept(detail(5, 1, 128)), "Reused batch ID accepted");
    require(!validator.accept(detail(5, 0, 128)), "Legacy mixed into v2 generation");
    auto dynamic = detail(5, 9, 128);
    dynamic.mutable_video_refinement()->set_kind(wire::DYNAMIC_DETAIL);
    require(validator.accept(dynamic).has_value(), "Monotonic batch gaps/dynamic kind rejected");
    require(!validator.accept(detail(6, 8, 0, 64)), "Descending batch ID accepted");
    require(validator.accept(detail(6, 10, 0, 64)).has_value(), "No final_batch should be needed");
    require(wire::VideoRefinement::descriptor()->FindFieldByName("final_batch") == nullptr, "Unexpected final_batch schema");
    require(validator.accept(frame(7, 1, 2)).has_value(), "Video blocked by refinement");
    auto next = detail(8, 10);
    next.set_frame_id(2);
    next.set_base_frame_id(2);
    require(!validator.accept(next), "Batch watermark reset on new frame");
    next.mutable_video_refinement()->set_batch_id(11);
    require(validator.accept(next).has_value(), "Grid not reset on new frame");
    auto region = envelope(9);
    region.set_generation(1);
    region.set_frame_id(3);
    region.set_base_frame_id(2);
    auto* update = region.mutable_desktop_region_update();
    update->set_target_frames_per_second(60);
    update->set_target_bitrate(1000000);
    auto* copy = update->add_copies();
    copy->mutable_destination()->set_width(128);
    copy->mutable_destination()->set_height(64);
    *update->add_patches() = detail().video_refinement().patches(0);
    update->mutable_patches(0)->set_cache_slot(1);
    require(validator.accept(region).has_value(), "Reliable cached region rejected in v2");
    next = detail(10, 12);
    next.set_frame_id(3);
    next.set_base_frame_id(3);
    require(validator.accept(next).has_value(), "Refinement must bind composed canvas, not latest video");
    region.set_sequence(11);
    region.set_frame_id(4);
    region.set_base_frame_id(3);
    region.mutable_desktop_region_update()->mutable_patches(0)->clear_png();
    require(validator.accept(region).has_value(), "v2 refinement changed reliable cache transaction");
    require(validator.accept(config(12, 2)).has_value(), "Generation reset failed");
    require(validator.accept(frame(13, 2)).has_value(), "New generation frame failed");
    next = detail(14);
    next.set_generation(2);
    require(validator.accept(next).has_value(), "Batch watermark not reset on generation");
    auto maxBatch = video();
    start(maxBatch);
    require(maxBatch.accept(detail(4, std::numeric_limits<std::uint64_t>::max())).has_value(), "uint64 maximum batch rejected");
    require(!maxBatch.accept(detail(5, 1, 128)), "Wrapped batch watermark accepted");
    auto edge = video();
    start(edge, config(2, 1, 2, 258, 130));
    next = detail();
    next.mutable_video_refinement()->clear_patches();
    patch(*next.mutable_video_refinement(), 256, 128, 2, 2);
    require(edge.accept(next).has_value(), "Canonical clipped edge rejected");
}
void verifyBudgets() {
    auto single = video();
    start(single, config(2, 1, 2, 8192, 4096));
    auto batch = detail();
    for (std::uint32_t index = 1; index < 33; ++index) {
        patch(*batch.mutable_video_refinement(), index * 128);
    }
    require(!single.accept(batch), "33-patch batch accepted");
    batch.mutable_video_refinement()->mutable_patches()->RemoveLast();
    require(single.accept(batch).has_value(), "32 patches/256Ki pixels rejected");
    auto bytes = video();
    start(bytes, config(2, 1, 2, 8192, 4096));
    for (std::uint32_t index = 0; index < 31; ++index) {
        batch = detail(index + 4, index + 1);
        batch.mutable_video_refinement()->clear_patches();
        for (std::uint32_t tile = 0; tile < 4; ++tile) {
            const auto cell = index * 4 + tile;
            patch(*batch.mutable_video_refinement(), (cell % 64) * 128, (cell / 64) * 64, 128, 64, 65536);
        }
        require(bytes.accept(batch).has_value(), "Within 8MiB cumulative bytes rejected");
    }
    batch = detail(35, 32);
    batch.mutable_video_refinement()->clear_patches();
    for (std::uint32_t cell = 124; cell < 127; ++cell) {
        patch(*batch.mutable_video_refinement(), (cell % 64) * 128, (cell / 64) * 64, 128, 64, 65536);
    }
    require(bytes.accept(batch).has_value(), "127 full-sized PNGs rejected");
    batch = detail(36, 33);
    batch.mutable_video_refinement()->clear_patches();
    patch(*batch.mutable_video_refinement(), 63 * 128, 64, 128, 64, 65536);
    patch(*batch.mutable_video_refinement(), 0, 128, 128, 64, 65536);
    require(!bytes.accept(batch), "Cumulative byte overflow accepted");
    batch.mutable_video_refinement()->mutable_patches()->RemoveLast();
    require(bytes.accept(batch).has_value(), "Failed cumulative validation mutated batch/grid/bytes/sequence");
    batch = detail(37, 34, 0, 128);
    require(!bytes.accept(batch), "Bytes beyond exact 8MiB accepted");
    require(bytes.accept(frame(37, 1, 2)).has_value(), "Failed bytes validation advanced sequence");
    batch.set_frame_id(2);
    batch.set_base_frame_id(2);
    batch.set_sequence(38);
    require(bytes.accept(batch).has_value(), "Byte budget not reset on baseline");
    auto tiles = video();
    start(tiles, config(2, 1, 2, 16384, 2048));
    for (std::uint32_t index = 0; index < 64; ++index) {
        batch = detail(index + 4, index + 1);
        batch.mutable_video_refinement()->clear_patches();
        for (std::uint32_t tile = 0; tile < 32; ++tile) {
            const auto cell = index * 32 + tile;
            patch(*batch.mutable_video_refinement(), (cell % 128) * 128, (cell / 128) * 64);
        }
        require(tiles.accept(batch).has_value(), "Within 2048 cumulative tiles rejected");
    }
    require(!tiles.accept(detail(68, 65, 0, 1024)), "2049 cumulative tiles accepted");
    auto hardBytes = detail();
    hardBytes.mutable_video_refinement()->clear_patches();
    for (std::uint32_t index = 0; index < 5; ++index) {
        patch(*hardBytes.mutable_video_refinement(), index * 128, 0, 128, 64, 65536);
    }
    require(!validateMessage(hardBytes), "Single batch compressed overflow accepted");
    hardBytes.mutable_video_refinement()->mutable_patches()->RemoveLast();
    require(validateMessage(hardBytes).has_value(), "256KiB hard bound rejected");
    hardBytes.mutable_video_refinement()->mutable_patches(0)->mutable_png()->push_back('x');
    require(!validateMessage(hardBytes), "Per-PNG 64KiB bound ignored");
}
// Reflection tests every duration field and its independent optional presence.
void checkDurationFields(wire::Envelope& value, google::protobuf::Message& timing, int fields, int first = 1) {
    for (int index = first; index <= fields; ++index) {
        const auto* field = timing.GetDescriptor()->FindFieldByNumber(index);
        const auto* reflection = timing.GetReflection();
        require(field && field->has_presence(), "Duration lacks presence");
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
wire::Envelope ack(std::uint64_t sequence = 2) {
    auto value = envelope(sequence, "input");
    value.set_generation(1);
    value.set_frame_id(1);
    value.mutable_desktop_refinement_ack()->set_batch_id(1);
    value.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::APPLIED);
    return value;
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
    auto refinement = detail();
    checkDurationFields(refinement, *refinement.mutable_video_refinement()->mutable_stage_timing(), 9);
    auto region = envelope(4);
    region.set_generation(1);
    region.set_frame_id(2);
    region.set_base_frame_id(1);
    auto* body = region.mutable_desktop_region_update();
    body->set_target_frames_per_second(60);
    body->set_target_bitrate(1000000);
    *body->add_patches() = detail().video_refinement().patches(0);
    checkDurationFields(region, *body->mutable_stage_timing(), 9);
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
    require(validateMessage(feedback).has_value(), "Legacy feedback rejected");
    auto diagnostic = report();
    checkDurationFields(diagnostic, *diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0), 7, 5);
    auto* sample = diagnostic.mutable_desktop_pipeline_report()->mutable_samples(0);
    sample->set_source(static_cast<wire::DesktopPipelineSample::Source>(99));
    require(!validateMessage(diagnostic), "Unknown diagnostic source accepted");
    sample->set_source(wire::DesktopPipelineSample::UNSPECIFIED);
    require(!validateMessage(diagnostic), "Unspecified diagnostic source accepted");
    sample->set_source(wire::DesktopPipelineSample::AGENT_RELAY);
    sample->set_batch_id(std::numeric_limits<std::uint64_t>::max());
    require(validateMessage(diagnostic).has_value(), "Relay/refinement sample rejected");
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
    refinement.mutable_video_refinement()->set_kind(static_cast<wire::RefinementKind>(99));
    require(!validateMessage(refinement), "Unknown kind accepted");
    refinement.mutable_video_refinement()->set_kind(wire::UNSPECIFIED);
    require(!validateMessage(refinement), "Batch without kind accepted");
    refinement.mutable_video_refinement()->set_batch_id(0);
    refinement.mutable_video_refinement()->set_kind(wire::STATIC_DETAIL);
    require(!validateMessage(refinement), "Kind without batch accepted");
    auto acknowledgment = ack();
    const auto malformed = [&](const wire::Envelope& item) { require(!validateMessage(item), "Malformed ACK accepted"); };
    acknowledgment.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::UNSPECIFIED);
    malformed(acknowledgment);
    acknowledgment.mutable_desktop_refinement_ack()->set_status(static_cast<wire::DesktopRefinementAck::Status>(99));
    malformed(acknowledgment);
    acknowledgment = ack();
    acknowledgment.mutable_desktop_refinement_ack()->set_batch_id(0);
    malformed(acknowledgment);
    acknowledgment = ack();
    acknowledgment.set_frame_id(0);
    malformed(acknowledgment);
    acknowledgment = ack();
    acknowledgment.set_generation(0);
    malformed(acknowledgment);
    acknowledgment = ack();
    acknowledgment.set_base_frame_id(1);
    malformed(acknowledgment);
    acknowledgment = ack();
    acknowledgment.clear_channel_id();
    malformed(acknowledgment);
    auto invalidHeader = detail();
    invalidHeader.mutable_video_refinement()->mutable_patches(0)->mutable_png()->at(0) = 'x';
    require(!validateMessage(invalidHeader), "Invalid PNG signature accepted");
    invalidHeader = detail();
    invalidHeader.mutable_video_refinement()->mutable_patches(0)->set_png(std::string(32, 'x'));
    require(!validateMessage(invalidHeader), "Truncated PNG header accepted");
    invalidHeader = detail();
    invalidHeader.mutable_video_refinement()->clear_patches();
    require(!validateMessage(invalidHeader), "Empty v2 batch accepted");
}
void verifyDirectionsAndIpc() {
    for (const auto source : {ChannelSource::kBrowser, ChannelSource::kAgent}) {
        for (const auto purpose : {wire::DESKTOP_INPUT, wire::DESKTOP_VIDEO}) {
            for (const bool diagnostic : {false, true}) {
                DesktopChannelValidator validator({"s", purpose == wire::DESKTOP_INPUT ? "input" : "video", purpose, source, false});
                auto value = diagnostic ? report() : ack();
                value.set_channel_id(purpose == wire::DESKTOP_INPUT ? "input" : "video");
                const bool allowed = purpose == wire::DESKTOP_INPUT && (diagnostic ? source == ChannelSource::kAgent : source == ChannelSource::kBrowser);
                const auto result = validator.accept(value);
                require(result.has_value() == allowed, "ACK/diagnostic direction whitelist failed");
                if (!allowed) {
                    require(result.error() == DesktopMessageError::kDirection, "Wrong direction error");
                }
            }
        }
    }
    DesktopChannelValidator browser({"s", "input", wire::DESKTOP_INPUT, ChannelSource::kBrowser, false});
    require(browser.accept(ack()).has_value(), "ACK on default/legacy gate should reach owner");
    require(browser.accept(ack(3)).has_value(), "Duplicate ACK tuple rejected");
    auto late = ack(4);
    late.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::SUPERSEDED);
    require(browser.accept(late).has_value(), "Late/retired ACK rejected");
    require(!browser.accept(late), "Sequence replay must still be rejected");
    for (const auto contract : {0u, 2u}) {
        DesktopPipeValidator desktop({"s", 7, false, DesktopPipeSource::kDesktop, contract});
        auto ready = envelope(1, "");
        ready.mutable_desktop_ready();
        require(desktop.accept(ready).has_value(), "IPC ready failed");
        require(desktop.accept(config()).has_value(), "IPC config failed");
        require(desktop.accept(frame()).has_value(), "IPC key failed");
        require(desktop.accept(detail()).has_value() == (contract == 2), "IPC binding gate did not propagate");
        desktop.setOptimizationContract(2);
        require(desktop.accept(config(5, 2)).has_value(), "IPC barrier failed");
        require(desktop.accept(frame(6, 2)).has_value(), "IPC new key failed");
        auto next = detail(7);
        next.set_generation(2);
        require(desktop.accept(next).has_value(), "IPC setter did not propagate");
        require(desktop.accept(report(8)).has_value(), "IPC worker diagnostic rejected");
        require(!desktop.accept(ack(9)), "IPC worker ACK accepted");
        DesktopPipeValidator service({"s", 7, false, DesktopPipeSource::kService});
        auto initial = envelope(1, "");
        initial.mutable_desktop_start()->set_windows_session_id(7);
        initial.mutable_desktop_start()->set_desktop_optimization_contract(contract);
        const auto encoded = encodeMessage(initial);
        require(encoded && decodeMessage(*encoded)->desktop_start().desktop_optimization_contract() == contract, "DesktopStart gate lost on wire");
        require(service.accept(initial).has_value(), "IPC service start failed");
        require(service.accept(ack()).has_value(), "IPC service ACK rejected");
        auto misplaced = report(9);
        misplaced.set_channel_id("video");
        require(!desktop.accept(misplaced), "IPC video diagnostic accepted");
        misplaced = ack(3);
        misplaced.set_channel_id("video");
        require(!service.accept(misplaced), "IPC video ACK accepted");
        require(!service.accept(report(3)), "IPC service diagnostic accepted");
        require(service.accept(ack(3)).has_value(), "Rejected IPC directions changed shared sequence");
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
        verifyBatches();
        verifyBudgets();
        verifyTimingAndMalformed();
        verifyDirectionsAndIpc();
        std::cout << "Desktop v2 gate, grid, budgets, atomic batches, timing presence, ACK/diagnostic directions and IPC passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
