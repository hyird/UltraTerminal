#include "tile_cache.transport.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "platform/windows/image_encoder.transport.h"

namespace ultraterimal::agent {
DesktopTileCache::DesktopTileCache() = default;
DesktopTileCache::~DesktopTileCache() = default;
bool DesktopTileCache::initializePng() {
    if (encoder_) {
        return false;
    }
    encoder_ = std::make_unique<DesktopPngEncoder>();
    return true;
}
std::optional<std::size_t> DesktopTileCache::predictedPngBytes(std::uint32_t width, std::uint32_t height) const {
    if (!pngDensityCount_ || !width || !height || width > 128 || height > 64) {
        return std::nullopt;
    }
    auto density = pngDensityEwma_;
    for (std::size_t i = 0; i < pngDensityCount_; ++i) {
        density = std::max(density, pngDensitySamples_[i]);
    }
    return static_cast<std::size_t>(std::clamp(std::ceil(density * width * height), 128.0, 64.0 * 1024 * 1024));
}
void DesktopTileCache::observePngBytes(std::size_t bytes, std::uint32_t pixels) {
    const auto density = static_cast<double>(bytes) / pixels;
    pngDensitySamples_[pngDensityNext_] = density;
    pngDensityNext_ = (pngDensityNext_ + 1) % pngDensitySamples_.size();
    pngDensityCount_ = std::min(pngDensityCount_ + 1, pngDensitySamples_.size());
    pngDensityEwma_ = pngDensityCount_ == 1 ? density : pngDensityEwma_ * 0.75 + density * 0.25;
}
void DesktopTileCache::clear() {
    reliable_ = {};
    local_ = {};
}
void DesktopTileCache::begin() {
    reliable_.pending.clear();
    reliable_.protectedSlots = {};
}
std::optional<wire::PatchRect> DesktopTileCache::prepare(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                                         const wire::Rectangle& area, bool cached, std::size_t byteBudget, DesktopRegionBudget* timeBudget, DesktopStageDurations* stages, DesktopTileObservation* observation) {
    return prepareInto(reliable_, bgra, width, height, area, cached, byteBudget, timeBudget, stages, observation);
}
std::optional<wire::PatchRect> DesktopTileCache::prepareLocal(std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                                              const wire::Rectangle& area, std::size_t byteBudget, DesktopRegionBudget* timeBudget, DesktopStageDurations* stages, DesktopTileObservation* observation) {
    auto patch = prepareInto(local_, bgra, width, height, area, false, byteBudget, timeBudget, stages, observation);
    commitStorage(local_);
    return patch;
}
std::optional<wire::PatchRect> DesktopTileCache::prepareInto(Storage& state, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height,
                                                             const wire::Rectangle& area, bool cached, std::size_t byteBudget, DesktopRegionBudget* timeBudget, DesktopStageDurations* stages, DesktopTileObservation* observation) {
    if (observation) {
        *observation = {};
    }
    const auto prepareStarted = DesktopRegionBudget::Clock::now();
    if (stages) {
        stages->prepareCpu.reset();
        stages->pngEncode.reset();
    }
    auto& entries_ = state.entries;
    auto& protected_ = state.protectedSlots;
    auto& pending_ = state.pending;
    auto& next_ = state.next;
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
        if (stages) {
            stages->prepareCpu = DesktopStageDurations::microseconds(prepareStarted, DesktopRegionBudget::Clock::now());
        }
        if (observation) {
            observation->pngBytes = cached && staged == pending_.end() && entry.defined ? 0 : entry.png.size();
            observation->reliableReference = cached && staged == pending_.end() && entry.defined;
            observation->localReuse = !cached;
        }
        if (cached) {
            if (staged != pending_.end() || !entry.defined) {
                if (entry.png.size() > byteBudget) {
                    return std::nullopt;
                }
                patch.set_png(entry.png);
                if (staged == pending_.end()) {
                    value.png = entry.png;
                    value.defined = true;
                    pending_.push_back({slot, std::move(value)});
                }
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
    const auto started = DesktopRegionBudget::Clock::now();
    if (stages) {
        stages->prepareCpu = DesktopStageDurations::microseconds(prepareStarted, started);
    }
    if (timeBudget && !timeBudget->canEncode(started)) {
        return std::nullopt;
    }
    initializePng();
    auto png = encodeDetailPng(*encoder_, value.pixels, area.width(), area.height());
    const auto finished = DesktopRegionBudget::Clock::now();
    if (stages) {
        stages->pngEncode = DesktopStageDurations::microseconds(started, finished);
    }
    if (observation) {
        observation->encoded = true;
        if (png) {
            observation->pngBytes = png->size();
        }
    }
    if (png) {
        // This owner sees the real codec product BEFORE time/byte/source
        // rejection. Never teach the cold-PNG model a cache reference's zero.
        observePngBytes(png->size(), area.width() * area.height());
    }
    if (timeBudget) {
        timeBudget->observe(std::chrono::duration_cast<std::chrono::microseconds>(finished - started), finished);
        if (!timeBudget->within(finished)) {
            return std::nullopt;
        }
    }
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
    commitStorage(reliable_);
}
void DesktopTileCache::commitStorage(Storage& state) {
    for (auto& definition : state.pending) {
        state.next = (definition.slot + 1) % state.entries.size();
        state.entries[definition.slot] = std::move(definition.entry);
    }
    state.pending.clear();
    state.protectedSlots = {};
}
} // namespace ultraterimal::agent
