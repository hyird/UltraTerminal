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
        require(!cache.predictedPngBytes(128, 64));
        agent::DesktopTileObservation observed;
        cache.begin();
        const auto staged = cache.prepare(pixels, 128, 64, area, true, 65536, nullptr, nullptr, &observed);
        require(staged && observed.encoded && observed.pngBytes && *observed.pngBytes == staged->png().size());
        require(cache.predictedPngBytes(128, 64) >= observed.pngBytes);
        require(staged && staged->cache_slot() && !staged->png().empty());
        const auto pendingReuse = cache.prepare(pixels, 128, 64, area, true, 65536, nullptr, nullptr, &observed);
        require(pendingReuse && pendingReuse->png() == staged->png() && !observed.reliableReference && !observed.encoded);
        require(!cache.prepare(pixels, 128, 64, area, true, 0)); // Uncommitted is not a free peer reference.
        const auto localStaged = cache.prepareLocal(pixels, 128, 64, area, 65536);
        require(localStaged && localStaged->cache_slot() == 0 && !localStaged->png().empty());
        const auto localReuse = cache.prepareLocal(pixels, 128, 64, area, 65536, nullptr, nullptr, &observed);
        require(observed.localReuse && !observed.encoded && observed.pngBytes == localStaged->png().size());
        require(localReuse && localReuse->png() == localStaged->png() && localReuse->cache_slot() == 0);
        require(!cache.prepareLocal(pixels, 128, 64, area, 1));
        cache.begin(); // Abandon an oversized batch; nothing reached the peer.
        const auto retry = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(retry && !retry->png().empty());
        // Local encoding of a different tile cannot publish/evict the staged
        // reliable definition, even when its own cache is cycled to eviction.
        auto other = pixels;
        for (unsigned color = 0; color < 255; ++color) {
            for (std::size_t index = 0; index < other.size(); index += 4) {
                other[index] = static_cast<std::uint8_t>(color);
            }
            require(cache.prepareLocal(other, 128, 64, area, 65536).has_value());
        }
        cache.commit();
        const auto fullPngPrediction = cache.predictedPngBytes(128, 64);
        auto reference = cache.prepare(pixels, 128, 64, area, true, 0, nullptr, nullptr, &observed);
        require(observed.reliableReference && observed.pngBytes == 0 && !observed.encoded);
        require(cache.predictedPngBytes(128, 64) == fullPngPrediction);
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
        std::fill(pixels.begin(), pixels.end(), std::uint8_t{255});
        auto restored = cache.prepare(pixels, 128, 64, area, true, 65536);
        require(restored && !restored->png().empty());
        cache.commit();
        cache.clear();
        require(cache.prepareLocal(pixels, 128, 64, area, 65536).has_value());
        require(!cache.prepare(pixels, 128, 64, area, true, 0)); // local PNG is not a remote definition
        cache.begin();
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
        // Real WIC output, not a byte quota: an oversized product must train
        // the predictor even though the entire reliable batch is abandoned.
        agent::DesktopTileCache oversized;
        oversized.begin();
        require(oversized.prepare(pixels, 128, 64, area, true, 65536).has_value());
        std::uint32_t random = 0x9e3779b9;
        auto entropy = pixels;
        for (std::size_t i = 0; i < entropy.size(); ++i) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            entropy[i] = i % 4 == 3 ? 255 : static_cast<std::uint8_t>(random);
        }
        agent::DesktopStageDurations failedStages;
        require(!oversized.prepare(entropy, 128, 64, area, true, 8333, nullptr, &failedStages, &observed));
        require(observed.encoded && observed.pngBytes && *observed.pngBytes > 20000);
        require(failedStages.pngEncode && failedStages.prepareCpu);
        require(oversized.predictedPngBytes(128, 64) >= observed.pngBytes);
        const auto rejectedPrediction = oversized.predictedPngBytes(128, 64);
        oversized.begin();
        const auto uncommitted = oversized.prepare(pixels, 128, 64, area, true, 65536);
        require(uncommitted && !uncommitted->png().empty()); // No partial remote commit.
        oversized.clear();
        require(oversized.predictedPngBytes(128, 64) >= rejectedPrediction); // Failed size retained.
        require(!oversized.prepare({}, 128, 64, area, false, 65536, nullptr, nullptr, &observed));
        require(!observed.encoded && !observed.pngBytes); // No artifact means unknown, not zero.
        std::cout << "Transactional publication, exact tile reuse, bounded eviction and generation reset passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
