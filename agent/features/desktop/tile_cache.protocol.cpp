#include "tile_cache.protocol.h"

#include <algorithm>
#include <cstring>

#include "platform/windows/image_encoder.transport.h"

namespace ultraterimal::agent {
void DesktopTileCache::clear() {
    entries_ = {};
    next_ = 0;
    begin();
}
void DesktopTileCache::begin() {
    pending_.clear();
    protected_ = {};
}
std::optional<wire::PatchRect> DesktopTileCache::prepare(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                                         const wire::Rectangle& area, bool cached, std::size_t byteBudget) {
    if (!area.width() || !area.height() || area.width() > 128 || area.height() > 64 || area.width() > width || area.height() > height ||
        area.x() > width - area.width() || area.y() > height - area.height() || bgra.size() != static_cast<std::size_t>(width) * height * 4) {
        return std::nullopt;
    }
    Entry value;
    value.width = area.width();
    value.height = area.height();
    value.pixels.resize(static_cast<std::size_t>(area.width()) * area.height() * 4);
    value.fingerprint = 14695981039346656037ull;
    for (std::uint32_t y = 0; y < area.height(); ++y) {
        auto* destination = value.pixels.data() + static_cast<std::size_t>(y) * area.width() * 4;
        std::memcpy(destination, bgra.data() + (static_cast<std::size_t>(area.y() + y) * width + area.x()) * 4, static_cast<std::size_t>(area.width()) * 4);
        for (std::uint32_t x = 0; x < area.width(); ++x) {
            destination[x * 4 + 3] = 255;
        }
        for (std::size_t x = 0; x < static_cast<std::size_t>(area.width()) * 4; ++x) {
            value.fingerprint = (value.fingerprint ^ destination[x]) * 1099511628211ull;
        }
    }
    const auto matches = [&](const Entry& entry) {
        return entry.fingerprint == value.fingerprint && entry.width == value.width && entry.height == value.height && entry.pixels == value.pixels;
    };
    wire::PatchRect patch;
    *patch.mutable_destination() = area;
    for (std::size_t slot = 0; slot < entries_.size(); ++slot) {
        const auto staged = std::find_if(pending_.begin(), pending_.end(), [slot](const Definition& definition) { return definition.slot == slot; });
        const auto& entry = staged != pending_.end() ? staged->entry : entries_[slot];
        if (!matches(entry)) {
            continue;
        }
        if (cached) {
            if (!entry.defined) {
                if (entry.png.size() > byteBudget) {
                    return std::nullopt;
                }
                value.png = entry.png;
                value.defined = true;
                patch.set_png(value.png);
                pending_.push_back({slot, std::move(value)});
            }
            patch.set_cache_slot(static_cast<std::uint32_t>(slot + 1));
            protected_[slot] = true;
        } else {
            if (entry.png.size() > byteBudget) {
                return std::nullopt;
            }
            patch.set_png(entry.png);
        }
        return patch;
    }
    auto png = encodeDetailPng(value.pixels, area.width(), area.height());
    if (!png || png->size() > byteBudget) {
        return std::nullopt;
    }
    value.png = std::move(*png);
    value.defined = cached;
    patch.set_png(value.png);
    for (std::size_t offset = 0; offset < entries_.size(); ++offset) {
        const auto slot = (next_ + offset) % entries_.size();
        if (!protected_[slot]) {
            protected_[slot] = true;
            if (cached) {
                patch.set_cache_slot(static_cast<std::uint32_t>(slot + 1));
            }
            pending_.push_back({slot, std::move(value)});
            return patch;
        }
    }
    return patch;
}
void DesktopTileCache::commit() {
    for (auto& definition : pending_) {
        next_ = (definition.slot + 1) % entries_.size();
        entries_[definition.slot] = std::move(definition.entry);
    }
    begin();
}
} // namespace ultraterimal::agent
