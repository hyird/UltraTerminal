#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "platform/windows/cursor.transport.h"

namespace {
using namespace ultraterimal;
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Cursor conversion invariant failed");
    }
}
agent::CapturedFrame screen() {
    agent::CapturedFrame frame;
    frame.width = frame.height = 4;
    frame.rotation = 1;
    frame.bgra.resize(4 * 4 * 4);
    for (std::size_t offset = 0; offset < frame.bgra.size(); offset += 4) {
        frame.bgra[offset] = 10;
        frame.bgra[offset + 1] = 20;
        frame.bgra[offset + 2] = 30;
        frame.bgra[offset + 3] = 255;
    }
    frame.pointerVisible = true;
    return frame;
}
void pixel(const wire::CursorUpdate& cursor, std::size_t index, std::array<unsigned, 4> expected) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        require(static_cast<unsigned char>(cursor.rgba().at(index * 4 + channel)) == expected[channel]);
    }
}
void valid(const wire::CursorUpdate& cursor) {
    wire::Envelope envelope;
    envelope.set_protocol_version(protocol::kVersion);
    envelope.set_session_id("cursor-test");
    envelope.set_channel_id("input");
    envelope.set_sequence(2);
    *envelope.mutable_cursor_update() = cursor;
    require(bool(protocol::validateMessage(envelope)));
}
void rejected(const agent::CapturedFrame& frame) {
    bool failed = false;
    try {
        agent::DesktopCursor cursor;
        (void)cursor.update(frame);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    require(failed);
}
void color() {
    auto frame = screen();
    frame.pointerType = 2;
    frame.pointerWidth = frame.pointerHeight = 2;
    frame.pointerPitch = 12; // Non-pixel row padding must not leak into RGBA.
    frame.hotspotX = frame.hotspotY = 1;
    frame.pointerX = -1;
    frame.pointerY = -2;
    frame.pointerShape = {10, 20, 30, 128, 40, 50, 60, 255, 9, 9, 9, 9,
                          70, 80, 90, 0, 100, 110, 120, 64, 9, 9, 9, 9};
    agent::DesktopCursor cursor;
    auto update = cursor.update(frame);
    require(update && update->x() == 0 && update->y() == -1 && update->hotspot_x() == 1 && update->width() == 2);
    pixel(*update, 0, {30, 20, 10, 128});
    pixel(*update, 1, {60, 50, 40, 255});
    pixel(*update, 2, {90, 80, 70, 0});
    pixel(*update, 3, {120, 110, 100, 64});
    valid(*update);
    require(!cursor.update(frame));
    frame.pointerX += 3;
    update = cursor.update(frame);
    require(update && update->x() == 3 && update->visible() && update->rgba().empty() && !update->width() && !update->hotspot_x());
    valid(*update);
    frame.pointerVisible = false;
    frame.pointerX = std::numeric_limits<std::int32_t>::min(); // Hidden coordinates are undefined.
    update = cursor.update(frame);
    require(update && !update->visible() && update->x() == 3 && update->rgba().empty());
    require(!cursor.update(frame));
    update = cursor.update(frame, true);
    require(update && !update->visible() && update->rgba().size() == 16);
    valid(*update);
    frame.pointerVisible = true;
    frame.pointerX = 2;
    update = cursor.update(frame);
    require(update && update->visible() && update->rgba().empty());
    update = cursor.update(frame, true);
    require(update && update->rgba().size() == 16 && update->hotspot_x() == 1);
    valid(*update);

    auto bad = frame;
    bad.pointerShape.pop_back();
    rejected(bad);
    bad = frame;
    bad.pointerPitch = 7;
    rejected(bad);
    bad = frame;
    bad.hotspotX = -1;
    rejected(bad);
    bad = frame;
    bad.hotspotY = 2;
    rejected(bad);
    bad = frame;
    bad.pointerX = std::numeric_limits<std::int32_t>::max();
    rejected(bad);
    bad = frame;
    bad.pointerWidth = 1025;
    rejected(bad);
    bad = frame;
    bad.pointerType = 3;
    rejected(bad);
    bad = frame;
    bad.bgra.clear();
    rejected(bad);
    bad = frame;
    bad.pointerPitch = std::numeric_limits<std::uint32_t>::max();
    rejected(bad);
    bad = frame;
    bad.width = bad.height = 0x80000000U;
    rejected(bad);
}
void monochrome() {
    auto frame = screen();
    frame.pointerType = 1;
    frame.pointerWidth = 4;
    frame.pointerHeight = 2; // AND row, then XOR row.
    frame.pointerPitch = 2;
    frame.pointerShape = {0x30, 0xcc, 0x50, 0xcc};
    agent::DesktopCursor cursor;
    auto update = cursor.update(frame);
    require(update && update->height() == 1);
    pixel(*update, 0, {0, 0, 0, 255});
    pixel(*update, 1, {255, 255, 255, 255});
    pixel(*update, 2, {0, 0, 0, 0});
    pixel(*update, 3, {225, 235, 245, 255});
    valid(*update);
    // Changing a transparent pixel does not change the shape.
    frame.bgra[2 * 4] = 0;
    require(!cursor.update(frame));
    // The inverted pixel must change even without a new DXGI shape or position.
    frame.bgra[3 * 4] = 0;
    update = cursor.update(frame);
    require(update && !update->rgba().empty());
    pixel(*update, 3, {225, 235, 255, 255});
    frame.pointerX = -4;
    update = cursor.update(frame);
    require(bool(update));
    pixel(*update, 3, {0, 0, 0, 0});
    frame.pointerX = 4;
    update = cursor.update(frame, true);
    pixel(*update, 3, {0, 0, 0, 0});
    auto bad = frame;
    bad.pointerHeight = 3;
    rejected(bad);
    bad = frame;
    bad.pointerPitch = 0;
    rejected(bad);
}
void masked() {
    auto frame = screen();
    frame.pointerType = 4;
    frame.pointerWidth = 3;
    frame.pointerHeight = 1;
    frame.pointerPitch = 12;
    frame.pointerShape = {1, 2, 3, 0, 255, 15, 240, 255, 0, 0, 0, 255};
    agent::DesktopCursor cursor;
    auto update = cursor.update(frame);
    require(bool(update));
    pixel(*update, 0, {3, 2, 1, 255});
    pixel(*update, 1, {238, 27, 245, 255});
    pixel(*update, 2, {0, 0, 0, 0});
    valid(*update);
    frame.pointerShape[3] = 128;
    rejected(frame);
    agent::DesktopCursor unknown;
    frame.pointerShape.clear();
    auto hidden = unknown.update(frame);
    require(hidden && !hidden->visible() && hidden->rgba().empty());
}
} // namespace
int main() {
    try {
        color();
        monochrome();
        masked();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Cursor color, AND/XOR, masked color, clipping, caching and bounds passed\n";
}
