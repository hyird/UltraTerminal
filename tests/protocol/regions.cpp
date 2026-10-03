#include <iostream>
#include <stdexcept>

#include "ultraterimal/protocol/desktop.protocol.h"

namespace {
using namespace ultraterimal;
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Region/cache contract failed");
    }
}
wire::Envelope message(std::uint64_t sequence, std::uint64_t frame = 0, std::uint64_t base = 0) {
    wire::Envelope value;
    value.set_protocol_version(1);
    value.set_session_id("s");
    value.set_channel_id("v");
    value.set_sequence(sequence);
    value.set_generation(1);
    value.set_frame_id(frame);
    value.set_base_frame_id(base);
    return value;
}
wire::Envelope configuration(std::uint64_t sequence, std::uint64_t generation) {
    auto value = message(sequence);
    value.set_generation(generation);
    value.mutable_video_config()->set_codec("avc1.42E01F");
    value.mutable_video_config()->set_width(256);
    value.mutable_video_config()->set_height(128);
    return value;
}
std::string png() {
    std::string value(33, '\0');
    value.replace(0, 8, "\211PNG\r\n\032\n", 8);
    value[11] = 13;
    value.replace(12, 4, "IHDR");
    value[19] = 128;
    value[23] = 64;
    return value; // Header validation only; real decoding is tested separately.
}
wire::Envelope region(std::uint64_t sequence, std::uint64_t frame, std::uint64_t base, bool define) {
    auto value = message(sequence, frame, base);
    auto* update = value.mutable_desktop_region_update();
    update->set_target_frames_per_second(60);
    update->set_target_bitrate(1000000);
    auto* patch = update->add_patches();
    patch->mutable_destination()->set_width(128);
    patch->mutable_destination()->set_height(64);
    patch->set_cache_slot(1);
    if (define) {
        patch->set_png(png());
    }
    return value;
}
} // namespace
int main() {
    try {
        protocol::DesktopChannelValidator video({"s", "v", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false});
        require(video.accept(configuration(2, 1)).has_value());
        auto key = message(3, 1);
        key.mutable_video_frame()->set_key_frame(true);
        key.mutable_video_frame()->set_data("key");
        require(video.accept(key).has_value());
        require(!video.accept(region(4, 2, 1, false))); // Unknown slot cannot alter state.
        auto bad = region(4, 2, 1, true);
        bad.mutable_desktop_region_update()->add_patches()->mutable_destination()->set_width(129);
        require(!video.accept(bad));
        require(!video.accept(region(4, 2, 1, false))); // Failed batch defined nothing.
        auto definition = region(4, 2, 1, true);
        const auto encoded = protocol::encodeMessage(definition);
        require(encoded && protocol::decodeMessage(*encoded));
        require(video.accept(*protocol::decodeMessage(*encoded)).has_value());
        auto reused = region(5, 3, 2, false);
        reused.mutable_desktop_region_update()->mutable_patches(0)->mutable_destination()->set_x(128);
        require(video.accept(reused).has_value());
        require(!video.accept(region(6, 4, 2, false))); // Stale canvas baseline.
        auto delta = message(6, 4);
        delta.mutable_video_frame()->set_data("delta");
        delta.mutable_video_frame()->set_timestamp_microseconds(1000);
        require(video.accept(delta).has_value()); // Region commits leave the codec timestamp/reference intact.
        auto copy = region(7, 5, 4, false);
        auto* moved = copy.mutable_desktop_region_update()->add_copies();
        moved->set_source_x(250);
        moved->mutable_destination()->set_width(128);
        moved->mutable_destination()->set_height(64);
        require(!video.accept(copy));
        moved->set_source_x(0);
        require(video.accept(copy).has_value());
        require(video.accept(configuration(8, 2)).has_value());
        key.set_sequence(9);
        key.set_generation(2);
        require(video.accept(key).has_value());
        reused = region(10, 2, 1, false);
        reused.set_generation(2);
        require(!video.accept(reused)); // Generation change retires every cache slot.
        protocol::DesktopChannelValidator input({"s", "i", wire::DESKTOP_INPUT, protocol::ChannelSource::kBrowser, false});
        auto capabilities = message(2);
        capabilities.set_channel_id("i");
        capabilities.set_generation(0);
        capabilities.mutable_desktop_video_capabilities()->add_codecs(wire::AVC);
        capabilities.mutable_desktop_video_capabilities()->add_codecs(wire::HEVC);
        capabilities.mutable_desktop_video_capabilities()->add_codecs(wire::AV1);
        require(input.accept(capabilities).has_value());
        capabilities.set_sequence(3);
        capabilities.mutable_desktop_video_capabilities()->add_codecs(wire::HEVC);
        require(!input.accept(capabilities));
        std::cout << "Atomic cache definitions, codec interleaving, stale/malformed updates, capability direction and generation retirement passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
