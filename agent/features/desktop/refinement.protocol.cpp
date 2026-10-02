#include "refinement.protocol.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "platform/windows/image_encoder.transport.h"

namespace ultraterimal::agent {
wire::VideoRefinement refineVideoDetails(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    wire::VideoRefinement result;
    if (!width || !height || width > 4096 || height > 2160 || bgra.size() != static_cast<std::size_t>(width) * height * 4) {
        return result;
    }
    struct Tile {
        std::uint32_t x, y, width, height, score;
    };
    std::vector<Tile> candidates;
    for (std::uint32_t y = 0; y < height; y += 64) {
        for (std::uint32_t x = 0; x < width; x += 128) {
            const auto tileWidth = std::min(128u, width - x), tileHeight = std::min(64u, height - y);
            std::array<bool, 4096> palette{};
            std::uint32_t colors = 0, edges = 0, flat = 0, samples = 0;
            for (std::uint32_t row = 0; row < tileHeight; row += 2) {
                for (std::uint32_t column = 0; column + 1 < tileWidth; column += 2) {
                    const auto offset = (static_cast<std::size_t>(y + row) * width + x + column) * 4;
                    const auto* pixel = bgra.data() + offset;
                    const auto color = static_cast<std::size_t>((pixel[0] >> 4) | ((pixel[1] >> 4) << 4) | ((pixel[2] >> 4) << 8));
                    colors += !palette[color];
                    palette[color] = true;
                    const auto distance = std::abs(static_cast<int>(pixel[0]) - pixel[4]) + std::abs(static_cast<int>(pixel[1]) - pixel[5]) + std::abs(static_cast<int>(pixel[2]) - pixel[6]);
                    edges += distance >= 160;
                    flat += distance <= 12;
                    ++samples;
                }
            }
            if (samples && colors <= 96 && flat * 2 >= samples && edges >= std::max(4u, samples / 100)) {
                candidates.push_back({x, y, tileWidth, tileHeight, edges * 1000 / samples});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Tile& left, const Tile& right) { return left.score > right.score; });
    std::size_t encodedBytes = 0;
    std::vector<std::uint8_t> pixels;
    for (const auto& tile : candidates) {
        if (result.patches_size() == 32) {
            break;
        }
        pixels.resize(static_cast<std::size_t>(tile.width) * tile.height * 4);
        for (std::uint32_t row = 0; row < tile.height; ++row) {
            auto* destination = pixels.data() + static_cast<std::size_t>(row) * tile.width * 4;
            std::memcpy(destination, bgra.data() + (static_cast<std::size_t>(tile.y + row) * width + tile.x) * 4, static_cast<std::size_t>(tile.width) * 4);
            for (std::uint32_t column = 0; column < tile.width; ++column) {
                destination[column * 4 + 3] = 255;
            }
        }
        auto png = encodeDetailPng(pixels, tile.width, tile.height);
        if (!png || encodedBytes + png->size() > 256 * 1024) {
            continue;
        }
        encodedBytes += png->size();
        auto* patch = result.add_patches();
        patch->mutable_destination()->set_x(tile.x);
        patch->mutable_destination()->set_y(tile.y);
        patch->mutable_destination()->set_width(tile.width);
        patch->mutable_destination()->set_height(tile.height);
        patch->set_png(std::move(*png));
    }
    return result;
}
} // namespace ultraterimal::agent
