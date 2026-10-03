#include "damage.protocol.h"

#include <algorithm>
#include <cstring>
#include <emmintrin.h>
#include <stdexcept>

namespace ultraterimal::agent {
namespace {
bool equalRgb(const std::uint8_t* current, const std::uint8_t* previous, std::uint32_t pixels) {
    const auto mask = _mm_set1_epi32(0x00ffffff);
    const auto zero = _mm_setzero_si128();
    std::uint32_t x = 0;
    for (; x + 4 <= pixels; x += 4) {
        const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(current + x * 4));
        const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(previous + x * 4));
        const auto difference = _mm_and_si128(_mm_xor_si128(a, b), mask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(difference, zero)) != 0xffff) {
            return false;
        }
    }
    for (; x < pixels; ++x) {
        if (current[x * 4] != previous[x * 4] || current[x * 4 + 1] != previous[x * 4 + 1] || current[x * 4 + 2] != previous[x * 4 + 2]) {
            return false;
        }
    }
    return true;
}
} // namespace
void DesktopDamage::reset(std::uint32_t width, std::uint32_t height) {
    width_ = width;
    height_ = height;
    columns_ = (width + 127) / 128;
    dirty_.assign(static_cast<std::size_t>(columns_) * ((height + 63) / 64), false);
    moved_.assign(dirty_.size(), false);
    baseline_.clear();
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
void DesktopDamage::forgetPixels() { baseline_.clear(); }
void DesktopDamage::rememberPixels(std::span<const std::uint8_t> bgra) {
    if (bgra.size() != static_cast<std::size_t>(width_) * height_ * 4) {
        throw std::invalid_argument("Incomplete desktop pixel baseline");
    }
    if (baseline_.size() != bgra.size() || !known_) {
        baseline_.assign(bgra.begin(), bgra.end());
        return;
    }
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        if (!dirty_[index] && !moved_[index]) {
            continue;
        }
        const auto x = static_cast<std::uint32_t>(index % columns_) * 128;
        const auto y = static_cast<std::uint32_t>(index / columns_) * 64;
        const auto rowBytes = static_cast<std::size_t>(std::min(128u, width_ - x)) * 4;
        for (std::uint32_t row = y; row < std::min(y + 64, height_); ++row) {
            const auto offset = (static_cast<std::size_t>(row) * width_ + x) * 4;
            std::memcpy(baseline_.data() + offset, bgra.data() + offset, rowBytes);
        }
    }
}
bool DesktopDamage::filterPixels(std::span<const std::uint8_t> bgra) {
    if (baseline_.empty() || baseline_.size() != bgra.size()) {
        return false;
    }
    std::vector<std::uint32_t> changed(dirty_.size());
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        if (known_ && !dirty_[index] && !moved_[index]) {
            continue;
        }
        const auto x = static_cast<std::uint32_t>(index % columns_) * 128;
        const auto y = static_cast<std::uint32_t>(index / columns_) * 64;
        for (std::uint32_t row = y; !changed[index] && row < std::min(y + 64, height_); ++row) {
            const auto offset = (static_cast<std::size_t>(row) * width_ + x) * 4;
            changed[index] = !equalRgb(bgra.data() + offset, baseline_.data() + offset, std::min(128u, width_ - x));
        }
    }
    filterTiles(changed);
    return true;
}
void DesktopDamage::filterTiles(std::span<const std::uint32_t> changed) {
    if (changed.size() != dirty_.size() || !updates_) {
        throw std::invalid_argument("Desktop tile comparison does not match its generation");
    }
    if (std::ranges::none_of(changed, [](auto value) { return value != 0; })) {
        std::fill(dirty_.begin(), dirty_.end(), false);
        std::fill(moved_.begin(), moved_.end(), false);
        copies_.clear();
        known_ = true;
        return;
    }
    if (!known_) {
        std::fill(dirty_.begin(), dirty_.end(), true);
        known_ = true; // The entire image was compared against a complete baseline.
    }
    for (std::size_t index = 0; index < dirty_.size(); ++index) {
        if (changed[index]) {
            continue;
        }
        const auto x = static_cast<std::uint32_t>(index % columns_) * 128;
        const auto y = static_cast<std::uint32_t>(index / columns_) * 64;
        // A copy can overwrite a dirty tile which finally equals the old image.
        // Keep its corrective patch unless the entire update was unchanged.
        const bool overwritten = std::ranges::any_of(copies_, [&](const auto& copy) {
            const auto& area = copy.destination();
            return x < area.x() + area.width() && area.x() < std::min(x + 128, width_) &&
                   y < area.y() + area.height() && area.y() < std::min(y + 64, height_);
        });
        if (!overwritten) {
            dirty_[index] = false;
        }
        if (copies_.empty()) {
            moved_[index] = false;
        }
    }
}
bool DesktopDamage::unchanged() const {
    return known_ && updates_ && copies_.empty() &&
           std::ranges::none_of(dirty_, [](bool value) { return value; }) &&
           std::ranges::none_of(moved_, [](bool value) { return value; });
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
