#include "capture_pixels.protocol.h"

#include <cstring>
#include <stdexcept>

namespace ultraterimal::agent {
namespace {
constexpr std::uint64_t maximumPixels = 64ull * 1024 * 1024 / 4;
}
void copyCapturePixels(std::span<std::uint8_t> target, std::uint32_t width, std::uint32_t height,
                       const void* source, std::size_t pitch) {
    if (!width || !height || static_cast<std::uint64_t>(width) * height > maximumPixels) {
        throw std::runtime_error("Invalid desktop dimensions");
    }
    const auto stride = static_cast<std::size_t>(width) * 4;
    if (!source || pitch < stride || target.size() < stride * height) {
        throw std::runtime_error("Invalid desktop pixel storage or row pitch");
    }
    const auto* pixels = static_cast<const std::uint8_t*>(source);
    for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(target.data() + row * stride, pixels + row * pitch, stride);
    }
}
} // namespace ultraterimal::agent
