#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ultraterimal/protocol/desktop.protocol.h"

namespace ultraterimal::agent {
// Encoding-worker owned. Definitions become reusable only when the entire
// ordered batch is committed; abandoning an oversized batch changes no slots.
class DesktopTileCache {
  public:
    void clear();
    void begin();
    std::optional<wire::PatchRect> prepare(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                           const wire::Rectangle& area, bool cached, std::size_t byteBudget);
    void commit();

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
    std::array<Entry, protocol::kDesktopTileCacheSlots> entries_;
    std::array<bool, protocol::kDesktopTileCacheSlots> protected_{};
    std::vector<Definition> pending_;
    std::size_t next_ = 0;
};
} // namespace ultraterimal::agent
