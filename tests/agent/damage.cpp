#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

#include "features/desktop/damage.protocol.h"

namespace {
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Desktop damage baseline or bound failed");
    }
}
ultraterimal::wire::Rectangle area(unsigned x, unsigned y, unsigned width, unsigned height) {
    ultraterimal::wire::Rectangle value;
    value.set_x(x);
    value.set_y(y);
    value.set_width(width);
    value.set_height(height);
    return value;
}
// Public state includes both dirty patches and move destinations; unknown
// cannot masquerade as empty known damage through a rejected comparison.
auto state(const ultraterimal::agent::DesktopDamage& damage) {
    std::vector<std::string> patches, copies;
    for (const auto& patch : damage.patches()) {
        patches.push_back(patch.SerializeAsString());
    }
    for (const auto& copy : damage.copies()) {
        copies.push_back(copy.SerializeAsString());
    }
    return std::tuple{damage.unchanged(), damage.regionCandidate(), damage.changedPixels(), damage.detailCandidates(), patches, copies};
}
void verifyComparedTiles() {
    using namespace ultraterimal;
    using Tile = agent::DesktopDamageComparisonTile;
    static_assert(std::is_aggregate_v<Tile> && std::is_trivially_copyable_v<Tile>);
    // No alpha/pixels/epoch in this value: producer checks exact RGB and
    // Runtime owns the baseline epoch/geometry, neither is invented here.
    agent::DesktopDamage damage;
    const auto reject = [&](std::span<const Tile> compared) {
        const auto before = state(damage);
        require(!damage.filterComparedTiles(compared));
        require(state(damage) == before);
    };
    damage.reset(258, 130);
    reject({}); // No acquired update.
    const std::array full{area(0, 0, 258, 130)};
    damage.add(true, {}, full);
    damage.add(true, {}, full); // Repeated dirty metadata, same final RGB.
    std::vector<Tile> zero;
    for (const auto& candidate : damage.detailCandidates()) {
        zero.push_back({candidate.x, candidate.y, candidate.width, candidate.height, 0});
    }
    require(zero.size() == 9);
    require(damage.filterComparedTiles(zero) && damage.unchanged());
    require(damage.patches().empty() && damage.copies().empty() && damage.changedPixels() == 0);
    damage.commit();
    damage.add(true, {}, full);
    // Order is immaterial; clipped edge geometry must match exactly.
    std::reverse(zero.begin(), zero.end());
    zero.front().changed = 1;
    require(damage.filterComparedTiles(zero));
    require(damage.detailCandidates() == std::vector<agent::DesktopDetailTile>{{256, 128, 2, 2}});
    require(damage.patches().size() == 1 && damage.changedPixels() == 4);

    damage.reset(1024, 512);
    const std::array allDirty{area(0, 0, 1024, 512)};
    damage.add(true, {}, allDirty);
    std::vector<Tile> allCounts;
    for (const auto& candidate : damage.detailCandidates()) {
        allCounts.push_back({candidate.x, candidate.y, candidate.width, candidate.height, 1});
    }
    require(allCounts.size() == 64 && damage.filterComparedTiles(allCounts));
    require(!damage.regionCandidate() && damage.changedPixels() == 1024 * 512);
    for (auto& tile : allCounts) {
        tile.changed = 0;
    }
    require(damage.filterComparedTiles(allCounts) && damage.unchanged());

    damage.reset(384, 64);
    wire::CopyRect copy;
    copy.set_source_x(0);
    copy.set_source_y(0);
    *copy.mutable_destination() = area(128, 0, 128, 64);
    const std::array moves{copy};
    const std::array dirty{area(128, 0, 128, 64), area(256, 0, 128, 64)};
    damage.add(true, moves, dirty);
    const std::array valid{Tile{128, 0, 128, 64, 0}, Tile{256, 0, 128, 64, 1}};
    reject({});
    reject(std::span(valid).first(1));                              // Missing C.
    reject(std::array{valid[0], valid[0]});                         // Duplicate B, missing C.
    reject(std::array{valid[0], Tile{256, 0, 127, 64, 1}});         // Partial tile.
    reject(std::array{valid[0], Tile{384, 0, 128, 64, 1}});         // Out of bounds.
    reject(std::array{valid[0], Tile{0, 0, 128, 64, 1}});           // Noncandidate A.
    reject(std::array{valid[0], Tile{256, 1, 128, 63, 1}});         // Shifted/subtile.
    reject(std::array{valid[0], Tile{256, 0, 128, 64, 2}});         // Not uint 0/1.
    reject(std::array{valid[0], valid[1], Tile{0, 0, 128, 64, 0}}); // Extra.
    require(damage.filterComparedTiles(valid));
    // A is red; B was blue, CopyRect makes B red, then dirty restores blue.
    // C really changed. B=0 C=1 still requires B's frozen corrective patch.
    require(damage.copies().size() == 1 && damage.patches().size() == 2);
    require(damage.patches()[0].x() == 128 && damage.patches()[1].x() == 256);
    // Cross-check the existing full RGB path with explicit red A / blue B.
    agent::DesktopDamage rgb;
    rgb.reset(384, 64);
    std::vector<std::uint8_t> pixels(384 * 64 * 4, 0);
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 384; ++x) {
            pixels[(y * 384 + x) * 4 + (x < 128 ? 2 : 0)] = 255;
        }
    }
    rgb.rememberPixels(pixels);
    pixels[256 * 4 + 1] = 255; // Only C really changes; B ends blue again.
    rgb.add(true, moves, dirty);
    require(rgb.filterPixels(pixels) && state(rgb) == state(damage));

    damage.commit();
    damage.add(true, moves, {}); // Move-only proof includes destination B.
    require(damage.filterComparedTiles(std::array{Tile{128, 0, 128, 64, 1}}));
    require(damage.copies().size() == 1 && damage.patches().empty());
    require(damage.filterComparedTiles(std::array{Tile{128, 0, 128, 64, 0}}));
    require(damage.unchanged() && damage.copies().empty());

    damage.commit();
    damage.add(true, moves, dirty);
    damage.add(true, moves, dirty); // Coalescing loses usable CopyRect.
    require(damage.copies().empty());
    require(damage.filterComparedTiles(valid));
    require(damage.patches().size() == 1 && damage.patches()[0].x() == 256);
    damage.commit();
    damage.add(true, moves, {});
    damage.add(true, moves, {});
    require(damage.filterComparedTiles(std::array{Tile{128, 0, 128, 64, 1}}));
    require(damage.copies().empty() && damage.patches().size() == 1 && damage.patches()[0].x() == 128);

    damage.commit();
    damage.add(true, {}, {}); // Empty expected set is not a comparison proof.
    reject({});
    reject(valid);
    damage.add(false, moves, dirty);
    const std::array unknownZero{Tile{128, 0, 128, 64, 0}, Tile{256, 0, 128, 64, 0}};
    reject({}); // Especially never recover unknown via an empty expected set.
    reject(unknownZero);
    reject(valid);
    damage.reset(384, 64);
    damage.add(true, moves, dirty);
    damage.add(false, {}, {}); // Unknown after accumulated dirty/moves.
    reject({});
    reject(valid);
    reject(unknownZero);
    require(!damage.unchanged() && !damage.regionCandidate() && damage.changedPixels() == 384 * 64);
}
} // namespace
int main() {
    using namespace ultraterimal;
    try {
        verifyComparedTiles();
        agent::DesktopDamage damage;
        damage.reset(1024, 512);
        wire::CopyRect copy;
        copy.set_source_x(0);
        copy.set_source_y(0);
        *copy.mutable_destination() = area(0, 64, 1024, 448);
        const std::array moves{copy};
        const std::array dirty{area(0, 0, 1024, 64)};
        damage.add(true, moves, dirty);
        require(damage.regionCandidate() && damage.copies().size() == 1 && damage.patches().size() == 8);
        require(damage.changedPixels() == 1024 * 512);
        require(damage.detailCandidates().size() == 64); // dirty plus copied destinations, >32 allowed
        // The second acquisition's moves refer to an unsent intermediate image.
        damage.add(true, moves, dirty);
        require(!damage.regionCandidate() && damage.copies().empty());
        damage.commit();
        require(!damage.regionCandidate() && damage.changedPixels() == 0 && damage.detailCandidates().empty());
        damage.add(false, {}, dirty);
        require(!damage.regionCandidate() && damage.changedPixels() == 1024 * 512 && damage.detailCandidates().empty());
        damage.reset(258, 130);
        const std::array edge{area(257, 129, 1, 1)};
        damage.add(true, {}, edge);
        auto patches = damage.patches();
        require(patches.size() == 1 && patches[0].x() == 256 && patches[0].y() == 128 && patches[0].width() == 2 && patches[0].height() == 2);
        require(damage.changedPixels() == 4);
        require(damage.detailCandidates() == std::vector<agent::DesktopDetailTile>{{256, 128, 2, 2}});
        damage.add(true, {}, edge);
        require(damage.regionCandidate() && damage.patches().size() == 1);
        const std::array invalid{area(258, 0, 1, 1)};
        damage.add(true, {}, invalid);
        require(!damage.regionCandidate() && damage.detailCandidates().empty());
        damage.reset(0, 0);
        require(damage.changedPixels() == 0 && !damage.regionCandidate());
        damage.reset(1024, 512);
        std::vector<std::uint8_t> pixels(1024 * 512 * 4, 255);
        damage.rememberPixels(pixels);
        const std::array coarse{area(0, 0, 1024, 512)};
        pixels[(100 * 1024 + 254) * 4] = 0;
        damage.add(true, {}, coarse);
        require(!damage.regionCandidate()); // DXGI overreports more than 32 tiles.
        require(damage.filterPixels(pixels) && damage.regionCandidate() && damage.patches().size() == 1);
        require(damage.changedPixels() == 128 * 64);
        damage.commit(); // Unsubmitted image: baseline must stay unchanged.
        damage.add(false, {}, {});
        require(damage.filterPixels(pixels) && damage.patches().size() == 1);
        damage.rememberPixels(pixels);
        damage.commit();
        for (std::size_t index = 3; index < pixels.size(); index += 4) {
            pixels[index] = 0;
        }
        damage.add(true, {}, coarse);
        require(damage.filterPixels(pixels) && damage.unchanged());
        damage.commit();
        damage.forgetPixels();
        damage.add(true, {}, coarse);
        require(!damage.filterPixels(pixels) && !damage.unchanged());
        damage.reset(1024, 512);
        pixels.assign(pixels.size(), 255);
        pixels[0] = 32;
        damage.rememberPixels(pixels);
        *copy.mutable_destination() = area(128, 0, 128, 64);
        pixels[(64 * 1024 + 512) * 4] = 48;
        const std::array corrective{area(128, 0, 128, 64), area(512, 64, 1, 1)};
        damage.add(true, std::array{copy}, corrective);
        require(damage.filterPixels(pixels) && damage.patches().size() == 2 && damage.copies().size() == 1);
        damage.reset(2, 2);
        std::vector<std::uint8_t> edgePixels(16, 255);
        damage.rememberPixels(edgePixels);
        edgePixels[12] = 0;
        damage.add(false, {}, {});
        require(damage.filterPixels(edgePixels) && damage.changedPixels() == 4 && damage.patches().size() == 1);
        std::cout << "Single-update scroll, coalescing, unknown damage and edge tiles passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
