#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Accumulates damage across coalesced raw snapshots. A copy is usable only for
// a single acquired update; otherwise every moved destination becomes a patch.
class DesktopDamage {
  public:
    void reset(std::uint32_t width, std::uint32_t height);
    void add(bool known, std::span<const wire::CopyRect> moves, std::span<const wire::Rectangle> dirty);
    void commit();
    std::uint64_t changedPixels() const;
    bool regionCandidate() const;
    std::vector<wire::Rectangle> patches() const;
    const std::vector<wire::CopyRect>& copies() const { return copies_; }

  private:
    void mark(const wire::Rectangle& area, std::vector<bool>& tiles);
    std::uint32_t width_ = 0, height_ = 0, columns_ = 0;
    std::uint32_t updates_ = 0;
    bool known_ = true;
    std::vector<bool> dirty_, moved_;
    std::vector<wire::CopyRect> copies_;
};
} // namespace ultraterimal::agent
