#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "features/desktop/tile_cache.transport.h"

namespace {
void require(bool value) {
    if (!value) {
        throw std::runtime_error("Desktop tile cache ownership or baseline failed");
    }
}
} // namespace
int main() {
    using namespace ultraterimal;
    try {
        agent::DesktopTileCache cache;
        wire::Rectangle area;
        area.set_width(128);
        area.set_height(64);
        std::vector<std::uint8_t> pixels(128 * 64 * 4, 255);
        cache.begin();
        const auto staged = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(staged && staged->cache_slot() && !staged->png().empty());
        cache.begin(); // Abandon an oversized batch; nothing reached the peer.
        const auto retry = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(retry && !retry->png().empty());
        cache.commit();
        auto reference = cache.prepare(pixels, 128, 64, area, true, 0);
        require(reference && reference->cache_slot() == retry->cache_slot() && reference->png().empty());
        cache.commit();
        // Alpha is normalized by the same PNG encoder and must not waste slots.
        for (std::size_t index = 3; index < pixels.size(); index += 4) {
            pixels[index] = 0;
        }
        reference = cache.prepare(pixels, 128, 64, area, true, 0);
        require(reference && reference->png().empty());
        cache.commit();
        for (unsigned color = 0; color < 255; ++color) {
            for (std::size_t index = 0; index < pixels.size(); index += 4) {
                pixels[index] = static_cast<std::uint8_t>(color);
            }
            auto definition = cache.prepare(pixels, 128, 64, area, true, 65536);
            require(definition && definition->cache_slot() <= protocol::kDesktopTileCacheSlots);
            cache.commit();
        }
        // The initial white tile has been evicted and must be defined again.
        std::fill(pixels.begin(), pixels.end(), 255);
        auto restored = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(restored && !restored->png().empty());
        cache.commit();
        cache.clear();
        require(!cache.prepare(pixels, 128, 64, area, true, 0));
        const auto uncached = cache.prepare(pixels, 128, 64, area, false, 65536);
        require(uncached && uncached->cache_slot() == 0 && !uncached->png().empty());
        cache.commit();
        const auto advertise = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(advertise && advertise->cache_slot() && !advertise->png().empty());
        require(!cache.prepare({}, 128, 64, area, true, 65536));
        agent::DesktopRegionBudget budget;
        using Clock = agent::DesktopRegionBudget::Clock;
        const auto start = Clock::time_point{} + std::chrono::seconds(10);
        budget.begin(start, 60);
        require(budget.canEncode(start) && budget.within(start + std::chrono::microseconds(3999)));
        require(!budget.within(start + std::chrono::microseconds(4000)));
        budget.observe(std::chrono::milliseconds(10), start);
        budget.begin(start + std::chrono::milliseconds(20), 60);
        require(!budget.canEncode(start + std::chrono::milliseconds(20)));
        budget.begin(start + std::chrono::seconds(1), 60);
        require(budget.canEncode(start + std::chrono::seconds(1)));
        budget.observe(std::chrono::microseconds(300), start + std::chrono::seconds(1));
        budget.begin(start + std::chrono::milliseconds(1020), 120);
        require(budget.canEncode(start + std::chrono::milliseconds(1020)));
        cache.clear();
        budget.begin(Clock::now() - std::chrono::seconds(1), 60);
        require(!cache.prepare(pixels, 128, 64, area, true, 65536, &budget));
        require(cache.prepare(pixels, 128, 64, area, true, 65536).has_value());
        cache.commit();
        require(cache.prepare(pixels, 128, 64, area, true, 0, &budget).has_value()); // Cached references bypass codec admission.
        std::cout << "Transactional publication, exact tile reuse, bounded eviction and generation reset passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
