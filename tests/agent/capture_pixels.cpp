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
        std::cout << "Complete BGRA copy, reusable storage and invalid row pitch passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
