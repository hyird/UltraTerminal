#include "refinement.runtime.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace ultraterimal::agent {
bool DesktopDetailPixels::valid() const {
    const auto rowBytes = static_cast<std::size_t>(width) * 4;
    return width && height && width <= 128 && height <= 64 && stride >= rowBytes && bgra.size() >= rowBytes &&
           (height == 1 || stride <= (bgra.size() - rowBytes) / (height - 1));
}
DesktopDetailScore classifyDesktopDetail(DesktopDetailPixels pixels) {
    DesktopDetailScore score;
    if (!pixels.valid()) {
        return score;
    }
    const auto edge = [](const std::uint8_t* a, const std::uint8_t* b) {
        const int blue = static_cast<int>(a[0]) - b[0];
        const int green = static_cast<int>(a[1]) - b[1];
        const int red = static_cast<int>(a[2]) - b[2];
        // RGB and opponent-color differences retain equal-luminance colored
        // strokes. A modest threshold includes antialiased/ClearType fringes.
        return std::abs(blue) + std::abs(green) + std::abs(red) >= 24 ||
               std::max(std::abs(red - green), std::abs(blue - green)) >= 16;
    };
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        for (std::uint32_t x = 0; x < pixels.width; ++x) {
            const auto* current = pixels.bgra.data() + y * pixels.stride + x * 4;
            if (x + 1 < pixels.width) {
                score.horizontalEdges += edge(current, current + 4);
                ++score.comparisons;
            }
            if (y + 1 < pixels.height) {
                score.verticalEdges += edge(current, current + pixels.stride);
                ++score.comparisons;
            }
        }
    }
    // No palette gate or subsampling: odd rows/columns and 1px strokes count.
    score.candidate = score.horizontalEdges + score.verticalEdges >= 2;
    return score;
}
void DesktopDetailTokens::refill(Clock::time_point now) {
    if (!initialized_ || now <= updated_) {
        return;
    }
    tokens_ = std::min(capacity_, tokens_ + std::chrono::duration<double>(now - updated_).count() * rate_);
    updated_ = now;
}
void DesktopDetailTokens::configure(Clock::time_point now, std::uint64_t throughputBytesPerSecond) {
    refill(now);
    throughput_ = throughputBytesPerSecond;
    rate_ = static_cast<double>(throughput_) * 0.2;
    capacity_ = std::min(65536.0, rate_ * 0.25); // bounded quarter-second burst
    tokens_ = std::min(tokens_, capacity_);      // no free credit on reconfiguration
    if (!initialized_) {
        updated_ = now;
        initialized_ = true;
    }
}
std::size_t DesktopDetailTokens::available(Clock::time_point now, std::size_t sendRemaining) {
    refill(now);
    // At most 50ms of link occupancy, even if many idle tokens accumulated.
    const auto occupancy = std::min(65536.0, static_cast<double>(throughput_) * 0.05);
    return std::min(sendRemaining, static_cast<std::size_t>(std::max(0.0, std::min(tokens_, occupancy))));
}
bool DesktopDetailTokens::consume(Clock::time_point now, std::size_t bytes, std::size_t sendRemaining) {
    if (bytes > available(now, sendRemaining)) {
        return false;
    }
    tokens_ -= static_cast<double>(bytes);
    return true;
}
bool DesktopRefinementPlan::begin(std::uint64_t generation, std::uint64_t frame, std::uint32_t width, std::uint32_t height, Clock::time_point now) {
    poll(now);
    if (!generation || !frame || generation < generation_ || !width || !height || width > 4096 || height > 4096 || static_cast<std::uint64_t>(width) * height > 4096ull * 2160 ||
        (active_ && generation == generation_ && frame == frame_ && width == width_ && height == height_)) {
        return false;
    }
    if (generation == generation_ && frame <= frame_) {
        return false; // a retired baseline cannot be restarted to repeat tiles
    }
    retire();
    if (generation != generation_) {
        lastBatch_ = 0;
        sliceSerial_ = 0;
        paused_ = false;
    }
    generation_ = generation;
    frame_ = frame;
    width_ = width;
    height_ = height;
    columns_ = (width + 127) / 128;
    remaining_ = columns_ * ((height + 63) / 64);
    visited_.assign(remaining_, false);
    eligible_.assign(remaining_, true);
    cursor_ %= remaining_;
    tileLimit_ = std::min(2048u, remaining_);
    sentTiles_ = 0;
    sentBytes_ = 0;
    active_ = true;
    return true;
}
void DesktopRefinementPlan::retire() {
    active_ = false;
    outstanding_.reset();
    remaining_ = 0;
    leasedTiles_ = 0;
}
bool DesktopRefinementPlan::resumeCanvas(std::uint64_t generation, std::uint64_t frame, std::uint32_t width, std::uint32_t height) {
    if (active_ || paused_ || generation != generation_ || frame != frame_ || width != width_ || height != height_ || visited_.empty()) {
        return false;
    }
    remaining_ = static_cast<std::uint32_t>(std::ranges::count(visited_, false));
    active_ = true;
    return true;
}
bool DesktopRefinementPlan::restrictTo(std::span<const DesktopDetailTile> damage) {
    if (!active_ || sentTiles_ || std::ranges::any_of(visited_, [](bool value) { return value; })) {
        return false;
    }
    for (const auto& area : damage) {
        if (!area.width || !area.height || area.width > width_ || area.height > height_ ||
            area.x > width_ - area.width || area.y > height_ - area.height) {
            return false;
        }
    }
    std::fill(eligible_.begin(), eligible_.end(), false);
    for (const auto& area : damage) {
        for (auto y = area.y / 64; y <= (area.y + area.height - 1) / 64; ++y) {
            for (auto x = area.x / 128; x <= (area.x + area.width - 1) / 128; ++x) {
                eligible_[static_cast<std::size_t>(y) * columns_ + x] = true;
            }
        }
    }
    return true;
}
void DesktopRefinementPlan::includeCanvas() {
    if (!active_) {
        return;
    }
    for (std::size_t index = 0; index < eligible_.size(); ++index) {
        if (!eligible_[index] && visited_[index]) {
            visited_[index] = false;
            ++remaining_;
        }
        eligible_[index] = true;
    }
}
bool DesktopRefinementPlan::reserveTiles(std::span<const DesktopDetailTile> tiles) {
    if (!active_ || paused_ || outstanding_ || tiles.empty() || tiles.size() > 8 || sliceSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    std::vector<std::uint32_t> indices;
    for (const auto& tile : tiles) {
        if (tile.x >= width_ || tile.y >= height_ || tile.x % 128 || tile.y % 64 ||
            tile.width != std::min(128u, width_ - tile.x) || tile.height != std::min(64u, height_ - tile.y)) {
            return false;
        }
        const auto index = tile.y / 64 * columns_ + tile.x / 128;
        if (visited_[index] || std::ranges::find(indices, index) != indices.end()) {
            return false;
        }
        indices.push_back(index);
    }
    for (const auto index : indices) {
        visited_[index] = true;
        --remaining_;
    }
    leasedTiles_ = static_cast<std::uint32_t>(tiles.size());
    cursor_ = (indices.back() + 1) % static_cast<std::uint32_t>(visited_.size());
    ++sliceSerial_;
    return true;
}
std::vector<DesktopDetailTile> DesktopRefinementPlan::nextSlice(Clock::time_point now, std::uint32_t maxTiles) {
    std::vector<DesktopDetailTile> result;
    poll(now);
    if (!active_ || paused_ || outstanding_ || complete() || sliceSerial_ == std::numeric_limits<std::uint64_t>::max()) {
        return result;
    }
    leasedTiles_ = 0;
    maxTiles = std::min({maxTiles, 8u, tileLimit_ - sentTiles_});
    for (unsigned inspected = 0; inspected < 32 && remaining_ && result.size() < maxTiles; ++inspected) {
        const auto index = cursor_;
        cursor_ = (cursor_ + 1) % static_cast<std::uint32_t>(visited_.size());
        if (visited_[index]) {
            continue;
        }
        visited_[index] = true;
        --remaining_;
        if (!eligible_[index]) {
            continue;
        }
        const auto x = index % columns_ * 128, y = index / columns_ * 64;
        result.push_back({x, y, std::min(128u, width_ - x), std::min(64u, height_ - y)});
    }
    leasedTiles_ = static_cast<std::uint32_t>(result.size());
    ++sliceSerial_;
    return result;
}
std::optional<DesktopDetailWorkKey> DesktopRefinementPlan::workKey() const {
    if (!active_ || paused_ || !leasedTiles_) {
        return std::nullopt;
    }
    return DesktopDetailWorkKey{generation_, frame_, sliceSerial_};
}
std::optional<DesktopDetailBatchKey> DesktopRefinementPlan::publish(Clock::time_point now, DesktopDetailWorkKey work, std::uint32_t tiles, std::size_t bytes, std::uint32_t pixels, std::uint64_t minimumBatch) {
    poll(now);
    if (!active_ || paused_ || workKey() != work || outstanding_ || !tiles || tiles > leasedTiles_ || tiles > 8 || !bytes || bytes > 65536 || !pixels || pixels > tiles * 8192 || pixels > 65536 ||
        tiles > tileLimit_ - sentTiles_ || bytes > 8 * 1024 * 1024 - sentBytes_ || lastBatch_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    lastBatch_ = std::max(lastBatch_ + 1, minimumBatch);
    leasedTiles_ = 0;
    outstanding_ = DesktopDetailBatchKey{generation_, frame_, lastBatch_};
    sentAt_ = now;
    sentTiles_ += tiles;
    sentBytes_ += bytes;
    return outstanding_;
}
bool DesktopRefinementPlan::acknowledge(Clock::time_point now, DesktopDetailBatchKey key, DesktopDetailAck status) {
    poll(now);
    if ((status != DesktopDetailAck::kApplied && status != DesktopDetailAck::kSuperseded) || !outstanding_ || key != *outstanding_) {
        return false;
    }
    outstanding_.reset();
    return true;
}
void DesktopRefinementPlan::rejectPublication(DesktopDetailBatchKey key) {
    if (outstanding_ == key) {
        outstanding_.reset();
    }
}
bool DesktopRefinementPlan::poll(Clock::time_point now) {
    if (outstanding_ && now - sentAt_ >= std::chrono::seconds(2)) {
        outstanding_.reset();
        paused_ = true;
        return true;
    }
    return false;
}
} // namespace ultraterimal::agent
