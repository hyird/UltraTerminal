#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "platform/windows/capture_pixels.protocol.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main() {
    try {
        using namespace ultraterimal::agent;
        constexpr std::uint32_t width = 8, height = 4;
        constexpr std::size_t stride = width * 4, pitch = stride + 12;
        std::vector<std::uint8_t> prior(stride * height);
        for (std::size_t index = 0; index < prior.size(); ++index) {
            prior[index] = static_cast<std::uint8_t>(index);
        }
        std::vector<std::uint8_t> current(pitch * height, 0xc0);
        for (std::size_t row = 0; row < height; ++row) {
            std::copy_n(prior.data() + row * stride, stride, current.data() + row * pitch);
        }
        auto rebuilt = prior;
        std::fill(current.begin(), current.end(), 0xe7);
        copyCapturePixels(rebuilt, width, height, current.data(), pitch);
        require(std::all_of(rebuilt.begin(), rebuilt.end(), [](auto value) { return value == 0xe7; }), "Full snapshot retained old pixels or copied padding");
        std::fill(current.begin(), current.end(), 0x19);
        copyCapturePixels(rebuilt, width, height, current.data(), pitch);
        require(std::all_of(rebuilt.begin(), rebuilt.end(), [](auto value) { return value == 0x19; }), "Reused storage retained an earlier snapshot");
        bool rejectedPitch = false;
        try {
            copyCapturePixels(rebuilt, width, height, current.data(), stride - 1);
        } catch (const std::runtime_error&) {
            rejectedPitch = true;
        }
        require(rejectedPitch, "Undersized desktop pitch was accepted");
        bool rejectedStorage = false;
        try {
            copyCapturePixels(std::span(rebuilt).first(1), width, height, current.data(), pitch);
        } catch (const std::runtime_error&) {
            rejectedStorage = true;
        }
        require(rejectedStorage, "Undersized desktop storage was accepted");
        std::fill(rebuilt.begin(), rebuilt.end(), 0x11);
        std::fill(current.begin(), current.end(), 0x88);
        const std::array<CapturePixelRect, 2> rects{{{2, 1, 5, 3}, {6, 0, 8, 2}}};
        copyCapturePixelRects(rebuilt, width, height, current.data(), pitch, rects);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const bool inside = (x >= 2 && x < 5 && y >= 1 && y < 3) || (x >= 6 && x < 8 && y < 2);
                const auto expected = static_cast<std::uint8_t>(inside ? 0x88 : 0x11);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    require(rebuilt[y * stride + x * 4 + channel] == expected, "Incremental rectangle copy corrupted outside or inside pixels");
                }
            }
        }
        bool rejectedRect = false;
        try {
            const std::array<CapturePixelRect, 1> invalid{{{3, 0, width + 1, 2}}};
            copyCapturePixelRects(rebuilt, width, height, current.data(), pitch, invalid);
        } catch (const std::runtime_error&) {
            rejectedRect = true;
        }
        require(rejectedRect, "Out-of-bounds desktop rectangle was accepted");
        std::cout << "Complete and incremental BGRA copy, reusable storage and invalid row pitch passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
