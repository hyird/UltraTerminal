#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "region_budget.types.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::agent {
class DesktopPngEncoder;
struct DesktopTileObservation {
    std::optional<std::size_t> pngBytes; // Known even when the produced PNG is rejected.
    bool encoded = false, reliableReference = false, localReuse = false;
};
// Encoding-worker owned. Definitions become reusable only when the entire
// ordered batch is committed; abandoning an oversized batch changes no slots.
class DesktopTileCache {
  public:
    DesktopTileCache();
    ~DesktopTileCache();
    DesktopTileCache(const DesktopTileCache&) = delete;
    DesktopTileCache& operator=(const DesktopTileCache&) = delete;
    DesktopTileCache(DesktopTileCache&&) = delete;
    DesktopTileCache& operator=(DesktopTileCache&&) = delete;
    // Initialize once before optional work is admitted, on the encoding worker.
    bool initializePng();
    void clear();
    void begin();
    // Metadata-only full-PNG prediction, never an unverified remote reference
    // or byte quota. Cache matching still happens once, inside prepare.
    std::optional<std::size_t> predictedPngBytes(std::uint32_t width, std::uint32_t height) const;
    std::optional<wire::PatchRect> prepare(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                           const wire::Rectangle& area, bool cached, std::size_t byteBudget, DesktopRegionBudget* timeBudget = nullptr, DesktopStageDurations* stages = nullptr, DesktopTileObservation* observation = nullptr);
    void commit();
    // Independent bounded local reuse for v2. Always returns a nonempty PNG
    // with cache_slot=0; never stages, commits or overwrites reliable slots.
    std::optional<wire::PatchRect> prepareLocal(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                                const wire::Rectangle& area, std::size_t byteBudget, DesktopRegionBudget* timeBudget = nullptr, DesktopStageDurations* stages = nullptr, DesktopTileObservation* observation = nullptr);

  private:
    struct Entry {
        std::uint64_t fingerprint = 0;
        std::uint32_t width = 0, height = 0;
        bool defined = false;
        std::vector<std::uint8_t> pixels;
        std::string png;
    };
    struct Definition {
        std::size_t slot;
        Entry entry;
    };
    struct Storage {
        std::array<Entry, protocol::kDesktopTileCacheSlots> entries;
        std::array<bool, protocol::kDesktopTileCacheSlots> protectedSlots{};
        std::vector<Definition> pending;
        std::size_t next = 0;
    };
    std::optional<wire::PatchRect> prepareInto(Storage& state, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                               const wire::Rectangle& area, bool cached, std::size_t byteBudget, DesktopRegionBudget* timeBudget, DesktopStageDurations* stages, DesktopTileObservation* observation);
    void observePngBytes(std::size_t bytes, std::uint32_t pixels);
    static void commitStorage(Storage& state);
    Storage reliable_, local_;
    std::unique_ptr<DesktopPngEncoder> encoder_;
    std::array<double, 8> pngDensitySamples_{};
    std::size_t pngDensityNext_ = 0, pngDensityCount_ = 0;
    double pngDensityEwma_ = 0;
};
} // namespace ultraterimal::agent
