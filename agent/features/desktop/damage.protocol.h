#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "refinement.runtime.h"
#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Frozen candidate geometry and exact RGB comparison against the submitted
// baseline. Alpha is ignored by the producer; changed must be 0 or 1.
struct DesktopDamageComparisonTile {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    std::uint32_t changed = 0;
};
// Accumulates damage across coalesced raw snapshots. A copy is usable only for
// a single acquired update; otherwise every moved destination becomes a patch.
class DesktopDamage {
  public:
    void reset(std::uint32_t width, std::uint32_t height);
    void add(bool known, std::span<const wire::CopyRect> moves, std::span<const wire::Rectangle> dirty);
    void commit();
    // Compare against the last submitted raw image, never an unsent capture.
    // False means that a complete baseline is not available yet.
    bool filterPixels(std::span<const std::uint8_t> bgra);
    void filterTiles(std::span<const std::uint32_t> changed);
    // Requires exactly all nonempty known detailCandidates(), in any order.
    // Rejection preserves pending damage/copies/known state. Retained copies
    // can require corrective patches from frozen, unchanged dirty candidates.
    bool filterComparedTiles(std::span<const DesktopDamageComparisonTile> compared);
    void rememberPixels(std::span<const std::uint8_t> bgra);
    void forgetPixels();
    bool unchanged() const;
    std::uint64_t changedPixels() const;
    bool regionCandidate() const;
    std::vector<wire::Rectangle> patches() const;
    // Known dirty/move tile destinations, independent of the reliable 32-patch
    // admission limit. Unknown damage returns no speculative pixel candidates.
    std::vector<DesktopDetailTile> detailCandidates() const;
    const std::vector<wire::CopyRect>& copies() const { return copies_; }

  private:
    void mark(const wire::Rectangle& area, std::vector<bool>& tiles);
    std::uint32_t width_ = 0, height_ = 0, columns_ = 0;
    std::uint32_t updates_ = 0;
    bool known_ = true;
    std::vector<bool> dirty_, moved_;
    std::vector<wire::CopyRect> copies_;
    std::vector<std::uint8_t> baseline_;
};
} // namespace ultraterimal::agent
