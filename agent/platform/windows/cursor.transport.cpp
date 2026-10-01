#include "cursor.transport.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include <windows.h>

#include <dxgi1_2.h>

namespace ultraterimal::agent {
namespace {
std::int32_t coordinate(std::int32_t origin, std::int32_t hotspot) {
    const auto result = static_cast<std::int64_t>(origin) + hotspot;
    if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("Cursor position exceeds range");
    }
    return static_cast<std::int32_t>(result);
}
void validateFrame(const CapturedFrame& frame) {
    const bool mono = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const bool color = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    const bool masked = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    const auto width = frame.pointerWidth, height = mono ? frame.pointerHeight / 2 : frame.pointerHeight;
    const auto minimumPitch = mono ? (static_cast<std::uint64_t>(width) + 7) / 8 : static_cast<std::uint64_t>(width) * 4;
    const auto required = static_cast<std::uint64_t>(frame.pointerPitch) * frame.pointerHeight;
    const auto screenBytes = static_cast<std::uint64_t>(frame.width) * frame.height * 4;
    if ((!mono && !color && !masked) || !width || !height || width > 1024 || height > 1024 || (mono && frame.pointerHeight % 2) ||
        frame.hotspotX < 0 || frame.hotspotY < 0 || static_cast<std::uint32_t>(frame.hotspotX) >= width || static_cast<std::uint32_t>(frame.hotspotY) >= height ||
        frame.pointerPitch < minimumPitch || required > 4 * 1024 * 1024 || required > frame.pointerShape.size() ||
        !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384 || screenBytes > 64 * 1024 * 1024 || screenBytes > frame.bgra.size() || frame.rotation != 1) {
        throw std::runtime_error("Invalid desktop cursor shape or background");
    }
}
wire::CursorUpdate convert(const CapturedFrame& frame) {
    const bool mono = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const bool masked = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    const auto width = frame.pointerWidth, height = mono ? frame.pointerHeight / 2 : frame.pointerHeight;
    wire::CursorUpdate result;
    result.set_visible(true);
    result.set_x(coordinate(frame.pointerX, frame.hotspotX));
    result.set_y(coordinate(frame.pointerY, frame.hotspotY));
    result.set_width(width);
    result.set_height(height);
    result.set_hotspot_x(static_cast<std::uint32_t>(frame.hotspotX));
    result.set_hotspot_y(static_cast<std::uint32_t>(frame.hotspotY));
    std::string rgba(static_cast<std::size_t>(width) * height * 4, '\0');
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::array<std::uint8_t, 4> pixel{};
            bool xorBackground = false;
            if (mono) {
                const auto offset = static_cast<std::size_t>(y) * frame.pointerPitch + x / 8;
                const auto bit = static_cast<std::uint8_t>(0x80U >> (x % 8));
                const bool andBit = (frame.pointerShape[offset] & bit) != 0;
                const bool xorBit = (frame.pointerShape[offset + static_cast<std::size_t>(height) * frame.pointerPitch] & bit) != 0;
                if (!andBit || xorBit) {
                    const auto value = static_cast<std::uint8_t>(xorBit ? 255 : 0);
                    pixel = {value, value, value, 255};
                    xorBackground = andBit;
                }
            } else {
                const auto offset = static_cast<std::size_t>(y) * frame.pointerPitch + x * 4;
                const auto* source = frame.pointerShape.data() + offset;
                pixel = {source[2], source[1], source[0], source[3]};
                if (masked) {
                    if (source[3] != 0 && source[3] != 255) {
                        throw std::runtime_error("Invalid masked cursor alpha");
                    }
                    xorBackground = source[3] == 255;
                    pixel[3] = 255;
                    if (xorBackground && !(pixel[0] | pixel[1] | pixel[2])) {
                        pixel = {};
                        xorBackground = false;
                    }
                }
            }
            if (xorBackground) {
                const auto screenX = static_cast<std::int64_t>(frame.pointerX) + x;
                const auto screenY = static_cast<std::int64_t>(frame.pointerY) + y;
                if (screenX < 0 || screenY < 0 || screenX >= frame.width || screenY >= frame.height) {
                    pixel = {};
                } else {
                    const auto offset = (static_cast<std::size_t>(screenY) * frame.width + static_cast<std::size_t>(screenX)) * 4;
                    pixel[0] ^= frame.bgra[offset + 2];
                    pixel[1] ^= frame.bgra[offset + 1];
                    pixel[2] ^= frame.bgra[offset];
                }
            }
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                rgba[offset + channel] = static_cast<char>(pixel[channel]);
            }
        }
    }
    result.set_rgba(std::move(rgba));
    return result;
}
} // namespace
std::optional<wire::CursorUpdate> DesktopCursor::update(const CapturedFrame& frame, bool force) {
    if (!frame.pointerVisible || frame.pointerShape.empty()) {
        // DXGI's hidden pointer coordinates are undefined. Keep the last known
        // hotspot, and wait for a real shape before showing a separate cursor.
        if (previous_ && !previous_->visible() && !force) {
            return std::nullopt;
        }
        if (!previous_) {
            previous_.emplace();
        }
        previous_->set_visible(false);
        if (force && !previous_->rgba().empty()) {
            return *previous_;
        }
        wire::CursorUpdate hidden;
        hidden.set_x(previous_->x());
        hidden.set_y(previous_->y());
        return hidden;
    }
    validateFrame(frame);
    if (frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR && previous_ && !previous_->rgba().empty() &&
        previous_->width() == frame.pointerWidth && previous_->height() == frame.pointerHeight &&
        previous_->hotspot_x() == static_cast<std::uint32_t>(frame.hotspotX) && previous_->hotspot_y() == static_cast<std::uint32_t>(frame.hotspotY) &&
        colorPitch_ == frame.pointerPitch && colorShape_ == frame.pointerShape) {
        const auto x = coordinate(frame.pointerX, frame.hotspotX);
        const auto y = coordinate(frame.pointerY, frame.hotspotY);
        if (previous_->visible() && previous_->x() == x && previous_->y() == y && !force) {
            return std::nullopt;
        }
        previous_->set_visible(true);
        previous_->set_x(x);
        previous_->set_y(y);
        if (force) {
            return *previous_;
        }
        wire::CursorUpdate position;
        position.set_visible(true);
        position.set_x(x);
        position.set_y(y);
        return position;
    }
    auto current = convert(frame);
    if (frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        colorShape_ = frame.pointerShape;
        colorPitch_ = frame.pointerPitch;
    } else {
        colorShape_.clear();
        colorPitch_ = 0;
    }
    const bool shapeChanged = !previous_ || current.width() != previous_->width() || current.height() != previous_->height() ||
                              current.hotspot_x() != previous_->hotspot_x() || current.hotspot_y() != previous_->hotspot_y() || current.rgba() != previous_->rgba();
    if (!shapeChanged && previous_->visible() && current.x() == previous_->x() && current.y() == previous_->y() && !force) {
        return std::nullopt;
    }
    previous_ = current;
    if (!shapeChanged && !force) {
        current.clear_rgba();
        current.clear_width();
        current.clear_height();
        current.clear_hotspot_x();
        current.clear_hotspot_y();
    }
    return current;
}
} // namespace ultraterimal::agent
