#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "refinement.runtime.h"
#include "region_budget.types.h"
#include "tile_cache.transport.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Legacy single-batch compatibility path, with its original palette selection.
// At most 32 tiles, 256 KiB encoded bytes and 256 Ki pixels per static snapshot.
wire::VideoRefinement refineVideoDetails(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height, DesktopTileCache* cache = nullptr, bool cached = false);
struct DesktopRefinementAttempt {
    std::optional<wire::PatchRect> patch;
    DesktopStageDurations stages;
    bool admitted = false;
    bool candidate = false;
    bool failed = false;
    DesktopTileObservation tile;
};
// V2 tile computation on one complete, source-tag-validated ROI. cost.begin()
// must precede optional platform work; an existing cost.admit() is reused.
// sendRemaining includes batch/envelope
// reservation by the caller. PNG credit is spent here, never refunded on
// retirement; only a current-source result may be published by the plan.
DesktopRefinementAttempt prepareVideoDetail(DesktopDetailPixels pixels, DesktopDetailTile destination, DesktopTileCache& cache,
                                            DesktopDetailTokens& tokens, DesktopRegionCost& cost, std::size_t sendRemaining,
                                            std::chrono::microseconds videoCost, std::optional<std::chrono::microseconds> estimatedSend, bool completeAttempt = true);
} // namespace ultraterimal::agent
