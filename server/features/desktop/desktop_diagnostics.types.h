#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ultraterimal::server {
struct DesktopEncodeSummary {
    std::size_t samples = 0;
    std::uint32_t meanMicroseconds = 0;
    std::uint32_t p95Microseconds = 0;
    std::uint32_t p99Microseconds = 0;
    std::uint32_t maximumMicroseconds = 0;
};
// Recent successful frames of the current video configuration, not a time
// window. No per-frame allocation; sort only when producing a log summary.
class DesktopEncodingWindow {
  public:
    void observe(std::uint32_t microseconds) {
        if (!microseconds) {
            return; // Older Agents have no measurement; never count it as zero latency.
        }
        values_[next_] = microseconds;
        next_ = (next_ + 1) % values_.size();
        count_ = std::min(count_ + 1, values_.size());
    }
    void clear() {
        count_ = 0;
        next_ = 0;
    }
    DesktopEncodeSummary summary() const {
        if (!count_) {
            return {};
        }
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            sum += sorted[i];
        }
        return {count_, static_cast<std::uint32_t>(sum / count_), sorted[(count_ * 95 + 99) / 100 - 1],
                sorted[(count_ * 99 + 99) / 100 - 1], sorted[count_ - 1]};
    }

  private:
    std::array<std::uint32_t, 1024> values_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};
struct DesktopDiagnostics {
    std::uint64_t forwardedMessages = 0;
    std::uint64_t forwardedBytes = 0;
    std::uint64_t videoFrames = 0;
    std::uint64_t inputEvents = 0;
    std::uint64_t feedbackMessages = 0;
    std::uint64_t backpressureEvents = 0;
    std::uint64_t backpressureMilliseconds = 0;
    std::size_t peakPendingBytes = 0;
    std::uint32_t maximumDecodeMicroseconds = 0;
    std::uint32_t maximumBrowserQueueDepth = 0;
    std::uint32_t latestBrowserQueueDepth = 0;
    bool latestBackground = false;
    DesktopEncodingWindow encoding;
    std::uint32_t videoWidth = 0;
    std::uint32_t videoHeight = 0;
    std::uint32_t latestKeyFrameEncodeMicroseconds = 0;
    bool latestHardwareEncoding = false;
};
} // namespace ultraterimal::server
