#include "capture_regions.protocol.h"

#include <cstring>
#include <stdexcept>

namespace ultraterimal::agent {
namespace {
constexpr std::uint64_t maximumPixels = 64ull * 1024 * 1024 / 4;
}
std::optional<std::uint64_t> collectCaptureRegions(std::uint32_t width, std::uint32_t height,
                                                   std::span<const DXGI_OUTDUPL_MOVE_RECT> moved, std::span<const RECT> dirty,
                                                   std::vector<RECT>& destinations) {
    destinations.clear();
    if (!width || !height || static_cast<std::uint64_t>(width) * height > maximumPixels) {
        return std::nullopt;
    }
    destinations.reserve(moved.size() + dirty.size());
    std::uint64_t area = 0;
    const auto include = [&](const RECT& rect) {
        if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
            static_cast<std::uint64_t>(rect.right) > width || static_cast<std::uint64_t>(rect.bottom) > height) {
            return false;
        }
        area += static_cast<std::uint64_t>(rect.right - rect.left) * (rect.bottom - rect.top);
        destinations.push_back(rect);
        return true;
    };
    for (const auto& move : moved) {
        const auto& rect = move.DestinationRect;
        if (!include(rect) || move.SourcePoint.x < 0 || move.SourcePoint.y < 0 ||
            static_cast<std::uint64_t>(move.SourcePoint.x) + (rect.right - rect.left) > width ||
            static_cast<std::uint64_t>(move.SourcePoint.y) + (rect.bottom - rect.top) > height) {
            destinations.clear();
            return std::nullopt;
        }
    }
    for (const auto& rect : dirty) {
        if (!include(rect)) {
            destinations.clear();
            return std::nullopt;
        }
    }
    return area;
}
void copyCapturePixels(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                       const void* source, std::size_t pitch, std::span<const RECT> destinations) {
    if (!width || !height || static_cast<std::uint64_t>(width) * height > maximumPixels) {
        throw std::runtime_error("Invalid desktop dimensions");
    }
    const auto stride = static_cast<std::size_t>(width) * 4;
    if (!source || pitch < stride || target.size() < stride * height) {
        throw std::runtime_error("Invalid desktop pixel storage or row pitch");
    }
    const auto* pixels = static_cast<const std::uint8_t*>(source);
    if (!destinations.empty()) {
        for (const auto& rect : destinations) {
            if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
                static_cast<std::uint64_t>(rect.right) > width || static_cast<std::uint64_t>(rect.bottom) > height) {
                throw std::runtime_error("Invalid desktop readback rectangle");
            }
            const auto left = static_cast<std::size_t>(rect.left) * 4;
            const auto bytes = static_cast<std::size_t>(rect.right - rect.left) * 4;
            for (auto row = rect.top; row < rect.bottom; ++row) {
                std::memcpy(target.data() + static_cast<std::size_t>(row) * stride + left,
                            pixels + static_cast<std::size_t>(row) * pitch + left, bytes);
            }
        }
        return;
    }
    for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(target.data() + row * stride, pixels + row * pitch, stride);
    }
}
} // namespace ultraterimal::agent
