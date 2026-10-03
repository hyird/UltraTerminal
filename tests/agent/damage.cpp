#include <array>
#include <iostream>
#include <stdexcept>
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
} // namespace
int main() {
    using namespace ultraterimal;
    try {
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
        // The second acquisition's moves refer to an unsent intermediate image.
        damage.add(true, moves, dirty);
        require(!damage.regionCandidate() && damage.copies().empty());
        damage.commit();
        require(!damage.regionCandidate() && damage.changedPixels() == 0);
        damage.add(false, {}, dirty);
        require(!damage.regionCandidate() && damage.changedPixels() == 1024 * 512);
        damage.reset(258, 130);
        const std::array edge{area(257, 129, 1, 1)};
        damage.add(true, {}, edge);
        auto patches = damage.patches();
        require(patches.size() == 1 && patches[0].x() == 256 && patches[0].y() == 128 && patches[0].width() == 2 && patches[0].height() == 2);
        require(damage.changedPixels() == 4);
        damage.add(true, {}, edge);
        require(damage.regionCandidate() && damage.patches().size() == 1);
        const std::array invalid{area(258, 0, 1, 1)};
        damage.add(true, {}, invalid);
        require(!damage.regionCandidate());
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
