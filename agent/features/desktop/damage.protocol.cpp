#include "damage.protocol.h"

#include <algorithm>

namespace ultraterimal::agent {
void DesktopDamage::reset(std::uint32_t width, std::uint32_t height) {
    width_ = width;
    height_ = height;
    columns_ = (width + 127) / 128;
    dirty_.assign(static_cast<std::size_t>(columns_) * ((height + 63) / 64), false);
    moved_.assign(dirty_.size(), false);
    commit();
}
void DesktopDamage::mark(const wire::Rectangle& area, std::vector<bool>& tiles) {
    if (!area.width() || !area.height() || area.width() > width_ || area.height() > height_ || area.x() > width_ - area.width() || area.y() > height_ - area.height()) {
        known_ = false;
        return;
    }
    for (auto y = area.y() / 64; y <= (area.y() + area.height() - 1) / 64; ++y) {
        for (auto x = area.x() / 128; x <= (area.x() + area.width() - 1) / 128; ++x) {
            tiles[static_cast<std::size_t>(y) * columns_ + x] = true;
        }
    }
}
void DesktopDamage::add(bool known, std::span<const wire::CopyRect> moves, std::span<const wire::Rectangle> dirty) {
    updates_ = std::min(2u, updates_ + 1);
    known_ &= known;
    if (!known_) {
        copies_.clear();
        return;
    }
    if (updates_ == 1 && moves.size() <= 32) {
        copies_.assign(moves.begin(), moves.end());
    } else {
        copies_.clear();
    }
    for (const auto& move : moves) {
        mark(move.destination(), moved_);
    }
    for (const auto& area : dirty) {
        mark(area, dirty_);
    }
}
void DesktopDamage::commit() {
    std::fill(dirty_.begin(), dirty_.end(), false);
    std::fill(moved_.begin(), moved_.end(), false);
    copies_.clear();
    updates_ = 0;
    known_ = true;
}
std::uint64_t DesktopDamage::changedPixels() const {
    if (!known_) {
        return static_cast<std::uint64_t>(width_) * height_;
    }
    std::uint64_t pixels = 0;
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        if (dirty_[index] || moved_[index]) {
            pixels += static_cast<std::uint64_t>(std::min(128u, width_ - static_cast<std::uint32_t>(index % columns_) * 128)) *
                      std::min(64u, height_ - static_cast<std::uint32_t>(index / columns_) * 64);
        }
    }
    return pixels;
}
bool DesktopDamage::regionCandidate() const {
    if (!known_ || !updates_) {
        return false;
    }
    std::size_t count = 0;
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        count += dirty_[index] || (copies_.empty() && moved_[index]);
    }
    return count <= 32 && (count || !copies_.empty());
}
std::vector<wire::Rectangle> DesktopDamage::patches() const {
    std::vector<wire::Rectangle> result;
    if (!regionCandidate()) {
        return result;
    }
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        if (dirty_[index] || (copies_.empty() && moved_[index])) {
            auto& area = result.emplace_back();
            area.set_x(static_cast<std::uint32_t>(index % columns_) * 128);
            area.set_y(static_cast<std::uint32_t>(index / columns_) * 64);
            area.set_width(std::min(128u, width_ - area.x()));
            area.set_height(std::min(64u, height_ - area.y()));
        }
    }
    return result;
}
} // namespace ultraterimal::agent
