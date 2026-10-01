#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "platform/windows/capture_regions.protocol.h"

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
        const DXGI_OUTDUPL_MOVE_RECT move{{0, 0}, {2, 0, 6, 2}};
        for (std::size_t row = 0; row < 2; ++row) {
            std::copy_n(prior.data() + row * stride, 4 * 4, current.data() + row * pitch + 2 * 4);
        }
        const std::array<RECT, 2> dirty = {RECT{3, 0, 5, 1}, RECT{1, 3, 4, 4}};
        for (const auto& rect : dirty) {
            for (auto row = rect.top; row < rect.bottom; ++row) {
                std::fill_n(current.data() + static_cast<std::size_t>(row) * pitch + rect.left * 4,
                            static_cast<std::size_t>(rect.right - rect.left) * 4, 0xe7);
            }
        }
        std::vector<RECT> regions;
        const auto area = collectCaptureRegions(width, height, std::span(&move, 1), dirty, regions);
        require(area == 13 && regions.size() == 3 && *area < width * height / 2, "Sparse move/dirty coverage was rejected");
        auto rebuilt = prior;
        copyCapturePixels(rebuilt, width, height, current.data(), pitch, regions);
        auto full = prior;
        copyCapturePixels(full, width, height, current.data(), pitch);
        require(rebuilt == full, "Overlapping move and dirty regions diverged from the full desktop");
        require(full != prior, "The test frame did not change");
        const DXGI_OUTDUPL_MOVE_RECT invalidMove{{7, 0}, {2, 0, 6, 2}};
        require(!collectCaptureRegions(width, height, std::span(&invalidMove, 1), dirty, regions) && regions.empty(), "Invalid move source was accepted");
        const std::array<RECT, 2> invalidDirty = {RECT{0, 0, 1, 1}, RECT{-1, 1, 2, 2}};
        require(!collectCaptureRegions(width, height, {}, invalidDirty, regions) && regions.empty(), "Invalid dirty rectangle was accepted");
        const RECT outside{0, 0, static_cast<LONG>(width + 1), 1};
        require(!collectCaptureRegions(width, height, {}, std::span(&outside, 1), regions) && regions.empty(), "Out-of-bounds rectangle was accepted");
        require(!collectCaptureRegions(0, height, {}, {}, regions) && regions.empty(), "Invalid desktop dimensions were accepted");
        require(collectCaptureRegions(width, height, {}, {}, regions) == 0 && regions.empty(), "Empty metadata was not preserved for full fallback");
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
        bool rejectedRegion = false;
        try {
            copyCapturePixels(rebuilt, width, height, current.data(), pitch, std::span(&outside, 1));
        } catch (const std::runtime_error&) {
            rejectedRegion = true;
        }
        require(rejectedRegion, "Out-of-bounds desktop copy was accepted");
        std::cout << "DXGI move/dirty reconstruction, full fallback and malformed metadata passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
