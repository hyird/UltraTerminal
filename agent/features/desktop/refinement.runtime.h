#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ultraterimal::agent {
struct DesktopDetailTile {
    std::uint32_t x = 0, y = 0, width = 0, height = 0;
    bool operator==(const DesktopDetailTile&) const = default;
};
// Every pixel in this view must be valid. A sparse full-canvas readback is not a
// view: pass each owned, complete ROI separately (including its row stride).
struct DesktopDetailPixels {
    std::span<const std::uint8_t> bgra;
    std::uint32_t width = 0, height = 0;
    std::size_t stride = 0;
    bool valid() const;
};
struct DesktopDetailScore {
    std::uint32_t horizontalEdges = 0, verticalEdges = 0, comparisons = 0;
    bool candidate = false;
};
DesktopDetailScore classifyDesktopDetail(DesktopDetailPixels pixels);

// Time-based, non-refundable optional-traffic credit. Neither frame changes nor
// retirement touch this state. Throughput is bytes/second, not bits/second.
class DesktopDetailTokens {
  public:
    using Clock = std::chrono::steady_clock;
    void configure(Clock::time_point now, std::uint64_t throughputBytesPerSecond);
    std::size_t available(Clock::time_point now, std::size_t sendRemaining);
    bool consume(Clock::time_point now, std::size_t bytes, std::size_t sendRemaining);

  private:
    void refill(Clock::time_point now);
    Clock::time_point updated_{};
    double rate_ = 0, capacity_ = 0, tokens_ = 0;
    std::uint64_t throughput_ = 0;
    bool initialized_ = false;
};

struct DesktopDetailBatchKey {
    std::uint64_t generation = 0, frame = 0, batch = 0;
    bool operator==(const DesktopDetailBatchKey&) const = default;
};
struct DesktopDetailWorkKey {
    std::uint64_t generation = 0, frame = 0, slice = 0;
    bool operator==(const DesktopDetailWorkKey&) const = default;
};
enum class DesktopDetailAck { kApplied,
                              kSuperseded };
// Single encoding-worker owner; no protocol or platform I/O. IDs are allocated
// only on publication. Empty/failed slices are legal and consume no ACK credit.
class DesktopRefinementPlan {
  public:
    using Clock = std::chrono::steady_clock;
    bool begin(std::uint64_t generation, std::uint64_t frame, std::uint32_t width, std::uint32_t height, Clock::time_point now);
    // Invalidates unsent work and the old key; retired/late ACKs cannot mutate a
    // replacement baseline. The spatial cursor and generation counter survive.
    void retire();
    // Only after complete-baseline RGB comparison proves the composed canvas
    // unchanged: preserve cursor, visited tiles, cumulative cost and pause.
    bool resumeCanvas(std::uint64_t generation, std::uint64_t frame, std::uint32_t width, std::uint32_t height);
    // For dynamic work, restrict candidates to known damage/move destinations.
    // Only intersected tiles are eligible; no full-image pixel scan is implied.
    bool restrictTo(std::span<const DesktopDetailTile> damage);
    // Once motion settles, continue the same baseline over the entire canvas.
    // Previously emitted/attempted eligible tiles remain visited.
    void includeCanvas();
    // Bind up to eight already selected, complete damage tiles to this baseline.
    // No scan is needed for a packed ROI result associated with a new video.
    bool reserveTiles(std::span<const DesktopDetailTile> tiles);
    // At most 32 grid positions inspected and 8 candidates returned per call.
    // A persistent spatial cursor provides fairness across baseline preemption.
    std::vector<DesktopDetailTile> nextSlice(Clock::time_point now, std::uint32_t maxTiles = 8);
    // Checks normal batch (8 / 64KiB / 64Ki pixels) and cumulative limits.
    // Caller must publish only PNG/cache_slot=0 patches from the current source.
    std::optional<DesktopDetailWorkKey> workKey() const;
    std::optional<DesktopDetailBatchKey> publish(Clock::time_point now, DesktopDetailWorkKey work, std::uint32_t tiles, std::size_t bytes, std::uint32_t pixels, std::uint64_t minimumBatch = 0);
    bool acknowledge(Clock::time_point now, DesktopDetailBatchKey key, DesktopDetailAck status);
    // Owner-loop admission rejected an UNSENT batch. Keep its spent bytes,
    // visited tiles and ID; this is not a fabricated peer ACK or a refund.
    void rejectPublication(DesktopDetailBatchKey key);
    bool poll(Clock::time_point now);
    bool paused() const { return paused_; }
    bool complete() const { return remaining_ == 0 || sentTiles_ >= tileLimit_ || sentBytes_ >= 8 * 1024 * 1024; }
    std::optional<DesktopDetailBatchKey> outstanding() const { return outstanding_; }
    std::uint32_t sentTiles() const { return sentTiles_; }
    std::size_t sentBytes() const { return sentBytes_; }

  private:
    std::uint64_t generation_ = 0, frame_ = 0, lastBatch_ = 0, sliceSerial_ = 0;
    std::uint32_t width_ = 0, height_ = 0, columns_ = 0, cursor_ = 0, remaining_ = 0, tileLimit_ = 0, sentTiles_ = 0, leasedTiles_ = 0;
    std::size_t sentBytes_ = 0;
    std::vector<bool> visited_, eligible_;
    std::optional<DesktopDetailBatchKey> outstanding_;
    Clock::time_point sentAt_{};
    bool active_ = false, paused_ = false;
};
} // namespace ultraterimal::agent
