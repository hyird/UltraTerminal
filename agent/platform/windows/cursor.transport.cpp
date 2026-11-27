#include "cursor.transport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <dxgi1_2.h>

#include "desktop.transport.h"

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
        !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384 || screenBytes > 64 * 1024 * 1024 || (!frame.bgra.empty() && screenBytes > frame.bgra.size()) || frame.rotation != 1) {
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
                    if (frame.bgra.size() < static_cast<std::size_t>(frame.width) * frame.height * 4) {
                        throw std::runtime_error("Desktop cursor inversion requires its captured background");
                    }
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
bool DesktopCursor::requiresBackground(const CapturedFrame& frame) {
    if (!frame.pointerVisible || frame.pointerShape.empty()) {
        return false;
    }
    validateFrame(frame);
    const bool mono = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const bool masked = frame.pointerType == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    if (!mono && !masked) {
        return false;
    }
    const auto height = mono ? frame.pointerHeight / 2 : frame.pointerHeight;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < frame.pointerWidth; ++x) {
            bool inversion;
            if (mono) {
                const auto offset = static_cast<std::size_t>(y) * frame.pointerPitch + x / 8;
                const auto bit = static_cast<std::uint8_t>(0x80U >> (x % 8));
                inversion = (frame.pointerShape[offset] & frame.pointerShape[offset + static_cast<std::size_t>(height) * frame.pointerPitch] & bit) != 0;
            } else {
                const auto* pixel = frame.pointerShape.data() + static_cast<std::size_t>(y) * frame.pointerPitch + x * 4;
                if (pixel[3] != 0 && pixel[3] != 255) {
                    throw std::runtime_error("Invalid masked cursor alpha");
                }
                inversion = pixel[3] == 255 && (pixel[0] | pixel[1] | pixel[2]);
            }
            const auto screenX = static_cast<std::int64_t>(frame.pointerX) + x;
            const auto screenY = static_cast<std::int64_t>(frame.pointerY) + y;
            if (inversion && screenX >= 0 && screenY >= 0 && screenX < frame.width && screenY < frame.height) {
                return true;
            }
        }
    }
    return false;
}
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

namespace {
constexpr std::array<UINT, 13> kSystemCursorIds = {
    32512, // OCR_NORMAL
    32513, // OCR_IBEAM
    32514, // OCR_WAIT
    32515, // OCR_CROSS
    32516, // OCR_UP
    32642, // OCR_SIZENWSE
    32643, // OCR_SIZENESW
    32644, // OCR_SIZEWE
    32645, // OCR_SIZENS
    32646, // OCR_SIZEALL
    32648, // OCR_NO
    32649, // OCR_HAND
    32650, // OCR_APPSTARTING
};

struct CachedCursorShape {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t hotspotX = 0;
    std::int32_t hotspotY = 0;
    std::vector<std::uint8_t> bgra;
};

struct ScopedGdiObject {
    HGDIOBJ handle = nullptr;
    ~ScopedGdiObject() {
        if (handle) {
            DeleteObject(handle);
        }
    }
};

struct ScopedScreenDc {
    HDC dc = GetDC(nullptr);
    ~ScopedScreenDc() {
        if (dc) {
            ReleaseDC(nullptr, dc);
        }
    }
};

bool readBitmapBgra(HDC dc, HBITMAP bitmap, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& out) {
    if (!dc || !bitmap || !width || !height || width > 1024 || height > 2048) {
        return false;
    }
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = static_cast<LONG>(width);
    header.bmiHeader.biHeight = -static_cast<LONG>(height);
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    out.resize(static_cast<std::size_t>(width) * height * 4);
    return GetDIBits(dc, bitmap, 0, height, out.data(), &header, DIB_RGB_COLORS) != 0;
}

std::optional<CachedCursorShape> extractCursorShape(HCURSOR cursor) {
    if (!cursor) {
        return std::nullopt;
    }
    ICONINFO info{};
    if (!GetIconInfo(cursor, &info)) {
        return std::nullopt;
    }
    ScopedGdiObject maskGuard{info.hbmMask};
    ScopedGdiObject colorGuard{info.hbmColor};
    ScopedScreenDc screenDc;
    if (!screenDc.dc) {
        return std::nullopt;
    }

    CachedCursorShape shape;
    if (info.hbmColor) {
        BITMAP colorBitmap{};
        if (!GetObjectW(info.hbmColor, sizeof(colorBitmap), &colorBitmap) || colorBitmap.bmWidth <= 0 || colorBitmap.bmHeight == 0) {
            return std::nullopt;
        }
        const auto width = static_cast<std::uint32_t>(colorBitmap.bmWidth);
        const auto height = static_cast<std::uint32_t>(std::abs(colorBitmap.bmHeight));
        if (width > 1024 || height > 1024) {
            return std::nullopt;
        }
        if (!readBitmapBgra(screenDc.dc, info.hbmColor, width, height, shape.bgra)) {
            return std::nullopt;
        }
        bool hasAlpha = false;
        for (std::size_t i = 3; i < shape.bgra.size(); i += 4) {
            if (shape.bgra[i] != 0) {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha && info.hbmMask) {
            std::vector<std::uint8_t> mask;
            if (readBitmapBgra(screenDc.dc, info.hbmMask, width, height, mask)) {
                for (std::size_t i = 0; i + 3 < shape.bgra.size(); i += 4) {
                    const bool transparent = (mask[i] | mask[i + 1] | mask[i + 2]) != 0;
                    if (transparent) {
                        shape.bgra[i] = 0;
                        shape.bgra[i + 1] = 0;
                        shape.bgra[i + 2] = 0;
                        shape.bgra[i + 3] = 0;
                    } else {
                        shape.bgra[i + 3] = 255;
                    }
                }
            }
        }
        shape.width = width;
        shape.height = height;
    } else if (info.hbmMask) {
        BITMAP maskBitmap{};
        if (!GetObjectW(info.hbmMask, sizeof(maskBitmap), &maskBitmap) || maskBitmap.bmWidth <= 0 || maskBitmap.bmHeight == 0) {
            return std::nullopt;
        }
        const auto width = static_cast<std::uint32_t>(maskBitmap.bmWidth);
        const auto fullHeight = static_cast<std::uint32_t>(std::abs(maskBitmap.bmHeight));
        if (width > 1024 || fullHeight < 2 || (fullHeight % 2) != 0 || fullHeight > 2048) {
            return std::nullopt;
        }
        const auto height = fullHeight / 2;
        std::vector<std::uint8_t> mask;
        if (!readBitmapBgra(screenDc.dc, info.hbmMask, width, fullHeight, mask)) {
            return std::nullopt;
        }
        shape.bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
        const std::size_t halfBytes = static_cast<std::size_t>(width) * height * 4;
        for (std::size_t i = 0; i + 3 < halfBytes; i += 4) {
            const bool andWhite = (mask[i] | mask[i + 1] | mask[i + 2]) != 0;
            const bool xorWhite = (mask[halfBytes + i] | mask[halfBytes + i + 1] | mask[halfBytes + i + 2]) != 0;
            if (!andWhite && !xorWhite) {
                shape.bgra[i] = 0;
                shape.bgra[i + 1] = 0;
                shape.bgra[i + 2] = 0;
                shape.bgra[i + 3] = 255;
            } else if (!andWhite && xorWhite) {
                shape.bgra[i] = 255;
                shape.bgra[i + 1] = 255;
                shape.bgra[i + 2] = 255;
                shape.bgra[i + 3] = 255;
            } else if (andWhite && xorWhite) {
                // Local CSS cursors cannot XOR against remote pixels; render
                // inverted pixels (such as I-beam stems) as solid dark ink.
                shape.bgra[i] = 0;
                shape.bgra[i + 1] = 0;
                shape.bgra[i + 2] = 0;
                shape.bgra[i + 3] = 255;
            }
        }
        shape.width = width;
        shape.height = height;
    } else {
        return std::nullopt;
    }

    shape.hotspotX = std::clamp<std::int32_t>(static_cast<std::int32_t>(info.xHotspot), 0, static_cast<std::int32_t>(shape.width - 1));
    shape.hotspotY = std::clamp<std::int32_t>(static_cast<std::int32_t>(info.yHotspot), 0, static_cast<std::int32_t>(shape.height - 1));
    return shape;
}

void populateFrameCursor(CapturedFrame& frame, const CachedCursorShape& shape, POINT screenPos, std::int32_t desktopLeft, std::int32_t desktopTop) {
    frame.pointerVisible = true;
    frame.pointerX = static_cast<std::int32_t>(screenPos.x) - desktopLeft - shape.hotspotX;
    frame.pointerY = static_cast<std::int32_t>(screenPos.y) - desktopTop - shape.hotspotY;
    frame.pointerType = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    frame.pointerWidth = shape.width;
    frame.pointerHeight = shape.height;
    frame.pointerPitch = shape.width * 4;
    frame.hotspotX = shape.hotspotX;
    frame.hotspotY = shape.hotspotY;
    frame.pointerShape = shape.bgra;
}
} // namespace

struct SystemCursorSuppression::State {
    bool suppressed = false;
    bool lastVisible = false;
    HCURSOR lastHandle = nullptr;
    std::unordered_map<HCURSOR, CachedCursorShape> systemShapes;
    std::unordered_map<HCURSOR, CachedCursorShape> customShapes;

    void activateSuppression() {
        if (suppressed) {
            return;
        }
        systemShapes.clear();
        for (const UINT id : kSystemCursorIds) {
            auto* handle = static_cast<HCURSOR>(LoadImageW(nullptr, MAKEINTRESOURCEW(id), IMAGE_CURSOR, 0, 0, LR_SHARED));
            if (!handle) {
                continue;
            }
            if (auto shape = extractCursorShape(handle)) {
                systemShapes.emplace(handle, std::move(*shape));
            }
        }
        constexpr BYTE kAndMask = 0xFF;
        constexpr BYTE kXorMask = 0x00;
        HCURSOR blank = CreateCursor(nullptr, 0, 0, 1, 1, &kAndMask, &kXorMask);
        if (!blank) {
            return;
        }
        bool replacedAny = false;
        for (const UINT id : kSystemCursorIds) {
            auto* copy = static_cast<HCURSOR>(CopyImage(blank, IMAGE_CURSOR, 0, 0, 0));
            if (!copy) {
                continue;
            }
            if (SetSystemCursor(copy, id)) {
                replacedAny = true;
            } else {
                DestroyCursor(copy);
            }
        }
        DestroyCursor(blank);
        suppressed = replacedAny;
    }

    const CachedCursorShape* resolveShape(HCURSOR handle) {
        if (const auto it = systemShapes.find(handle); it != systemShapes.end()) {
            return &it->second;
        }
        if (const auto it = customShapes.find(handle); it != customShapes.end()) {
            return &it->second;
        }
        if (auto shape = extractCursorShape(handle)) {
            if (customShapes.size() >= 32) {
                customShapes.clear();
            }
            auto [inserted, _] = customShapes.emplace(handle, std::move(*shape));
            return &inserted->second;
        }
        return nullptr;
    }
};

SystemCursorSuppression::SystemCursorSuppression() : state_(std::make_unique<State>()) {}

SystemCursorSuppression::~SystemCursorSuppression() {
    restore();
}

bool SystemCursorSuppression::active() const {
    return state_ && state_->suppressed;
}

void SystemCursorSuppression::applyFallbackCursor(CapturedFrame& frame, std::int32_t desktopLeft, std::int32_t desktopTop) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
        state_->lastVisible = false;
        state_->lastHandle = nullptr;
        return;
    }
    state_->activateSuppression();
    state_->lastVisible = true;
    state_->lastHandle = ci.hCursor;
    if (const auto* shape = state_->resolveShape(ci.hCursor)) {
        populateFrameCursor(frame, *shape, ci.ptScreenPos, desktopLeft, desktopTop);
    }
}

bool SystemCursorSuppression::pollCursorChange(CapturedFrame& frame, std::int32_t desktopLeft, std::int32_t desktopTop) {
    if (!state_ || !state_->suppressed) {
        return false;
    }
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    const bool visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor != nullptr;
    HCURSOR handle = visible ? ci.hCursor : nullptr;
    if (visible == state_->lastVisible && handle == state_->lastHandle) {
        return false;
    }
    state_->lastVisible = visible;
    state_->lastHandle = handle;
    if (!visible) {
        frame.pointerVisible = false;
        frame.pointerShape.clear();
        return true;
    }
    if (const auto* shape = state_->resolveShape(handle)) {
        populateFrameCursor(frame, *shape, ci.ptScreenPos, desktopLeft, desktopTop);
        return true;
    }
    return false;
}

void SystemCursorSuppression::restore() {
    if (!state_ || !state_->suppressed) {
        return;
    }
    try {
        (void)requireInputDesktop();
    } catch (...) {
    }
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    state_->suppressed = false;
    state_->lastVisible = false;
    state_->lastHandle = nullptr;
    state_->systemShapes.clear();
    state_->customShapes.clear();
}
} // namespace ultraterimal::agent
