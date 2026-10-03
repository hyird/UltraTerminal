#include <array>
#include <iostream>
#include <stdexcept>

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
        std::cout << "Single-update scroll, coalescing, unknown damage and edge tiles passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
