#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ultraterimal/protocol/message.protocol.h"

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
template <std::size_t Capacity>
class DesktopDurationWindow {
  public:
    void observe(std::uint32_t microseconds) {
        if (microseconds) {
            observePresent(microseconds); // Legacy scalar zero means unavailable.
        }
    }
    void observePresent(std::uint32_t microseconds) {
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
    std::array<std::uint32_t, Capacity> values_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};
using DesktopEncodingWindow = DesktopDurationWindow<1024>;
// These one-second browser reports contain overlapping windows of at most
// 120 rAF intervals. The latest and worst snapshots are meaningful; summing
// long-gap counts across reports would count the same interval repeatedly.
struct DesktopVisibleFrameDiagnostics {
    std::uint64_t reports = 0;
    std::uint32_t latestSamples = 0;
    std::uint32_t latestP95Microseconds = 0;
    std::uint32_t latestP99Microseconds = 0;
    std::uint32_t latestLongGaps = 0;
    std::uint32_t latestFpsMilli = 0;
    std::uint32_t minimumFpsMilli = 0;
    std::uint32_t maximumFpsMilli = 0;
    std::uint32_t maximumP95Microseconds = 0;
    std::uint32_t maximumP99Microseconds = 0;
    std::uint32_t maximumLongGaps = 0;

    void observe(std::uint32_t samples, std::uint32_t p95Microseconds, std::uint32_t p99Microseconds, std::uint32_t longGaps, std::uint32_t fpsMilli) {
        if (!reports || fpsMilli < minimumFpsMilli) {
            minimumFpsMilli = fpsMilli;
        }
        ++reports;
        latestSamples = samples;
        latestP95Microseconds = p95Microseconds;
        latestP99Microseconds = p99Microseconds;
        latestLongGaps = longGaps;
        latestFpsMilli = fpsMilli;
        maximumFpsMilli = std::max(maximumFpsMilli, fpsMilli);
        maximumP95Microseconds = std::max(maximumP95Microseconds, p95Microseconds);
        maximumP99Microseconds = std::max(maximumP99Microseconds, p99Microseconds);
        maximumLongGaps = std::max(maximumLongGaps, longGaps);
    }
    void clear() { *this = {}; }
};
// Fixed allowlist. Each percentile describes ONE local stage, never end-to-end
// latency. Browser submit/write completion are software milestones, not display.
enum class DesktopStage : std::size_t {
    kWorkerQueue,
    kAcquireWait,
    kSourceAge,
    kPacingCreditWait,
    kGpuSyncWait,
    kReadbackReady,
    kPrepareCpu,
    kPngEncode,
    kVideoEncode,
    kBrowserReceiveQueue,
    kBrowserDecode,
    kBrowserCanvasSubmit,
    kBrowserPngDecode,
    kBrowserBaselineWait,
    kWorkerIpcWrite,
    kRelaySendQueueWait,
    kRelayNativeWrite,
    kServerVideoAdmission,
    kServerVideoQueueWait,
    kServerVideoWsWrite,
    kCount
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(DesktopStage::kCount)> kDesktopStageNames{
    "worker_queue", "acquire_wait", "source_age", "pacing_credit_wait", "gpu_sync_wait",
    "readback_ready", "prepare_cpu", "png_encode", "video_encode",
    "browser_receive_queue", "browser_decode", "browser_canvas_submit", "browser_png_decode", "browser_baseline_wait",
    "worker_ipc_write", "relay_send_queue_wait", "relay_native_write",
    "server_video_admission", "server_video_queue_wait", "server_video_ws_write"};
struct DesktopFrameKey {
    std::uint64_t generation = 0, frame = 0, batch = 0;
    bool operator==(const DesktopFrameKey&) const = default;
};
class DesktopPipelineDiagnostics {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::uint32_t kMaximumDuration = 60000000;
    void observe(DesktopStage stage, std::uint32_t value) {
        if (value <= kMaximumDuration) {
            stages[static_cast<std::size_t>(stage)].observePresent(value);
        }
    }
    void observe(const wire::DesktopStageTiming& timing) {
#define DESKTOP_STAGE(field, stage)                   \
    if (timing.has_##field()) {                       \
        observe(DesktopStage::stage, timing.field()); \
    }
        DESKTOP_STAGE(worker_queue_microseconds, kWorkerQueue)
        DESKTOP_STAGE(acquire_wait_microseconds, kAcquireWait)
        DESKTOP_STAGE(source_age_microseconds, kSourceAge)
        DESKTOP_STAGE(pacing_credit_wait_microseconds, kPacingCreditWait)
        DESKTOP_STAGE(gpu_sync_wait_microseconds, kGpuSyncWait)
        DESKTOP_STAGE(readback_ready_microseconds, kReadbackReady)
        DESKTOP_STAGE(prepare_cpu_microseconds, kPrepareCpu)
        DESKTOP_STAGE(png_encode_microseconds, kPngEncode)
        DESKTOP_STAGE(video_encode_microseconds, kVideoEncode)
#undef DESKTOP_STAGE
    }
    void observe(const wire::DesktopBrowserTiming& timing) {
#define DESKTOP_STAGE(field, stage)                   \
    if (timing.has_##field()) {                       \
        observe(DesktopStage::stage, timing.field()); \
    }
        DESKTOP_STAGE(receive_queue_microseconds, kBrowserReceiveQueue)
        DESKTOP_STAGE(decode_microseconds, kBrowserDecode)
        DESKTOP_STAGE(canvas_submit_microseconds, kBrowserCanvasSubmit)
        DESKTOP_STAGE(png_decode_microseconds, kBrowserPngDecode)
        DESKTOP_STAGE(baseline_wait_microseconds, kBrowserBaselineWait)
#undef DESKTOP_STAGE
    }
    bool known(DesktopFrameKey key) const {
        return key.generation && key.frame && std::ranges::find(known_, key) != known_.end();
    }
    void frame(DesktopFrameKey key, Clock::time_point now = Clock::now()) {
        expire(now);
        known_[nextKnown_] = key;
        nextKnown_ = (nextKnown_ + 1) % known_.size();
        for (auto& entry : pending_) {
            if (entry && entry->key == key) {
                observe(*entry);
                entry.reset();
            }
        }
    }
    void generation(std::uint64_t value, Clock::time_point now = Clock::now()) {
        generation_ = value;
        known_ = {};
        nextKnown_ = 0;
        expire(now);
        for (auto& entry : pending_) {
            if (entry && entry->key.generation < value) {
                entry.reset();
                ++dropped;
            }
        }
    }
    void report(const wire::DesktopPipelineReport& report, Clock::time_point now = Clock::now()) {
        expire(now);
        if (report.samples_size() > 32 || (lastReport_ && now - *lastReport_ < std::chrono::milliseconds(500))) {
            dropped += report.samples_size();
            return;
        }
        lastReport_ = now;
        dropped += report.dropped_samples();
        for (const auto& sample : report.samples()) {
            const DesktopFrameKey key{sample.generation(), sample.frame_id(), sample.batch_id()};
            // Source/field combinations are closed, not a generic trace surface.
            const bool worker = sample.source() == wire::DesktopPipelineSample::DESKTOP_WORKER;
            const bool relay = sample.source() == wire::DesktopPipelineSample::AGENT_RELAY;
            if (!key.generation || !key.frame || key.generation < generation_ || (!worker && !relay) ||
                (worker && (sample.has_send_queue_wait_microseconds() || sample.has_native_write_microseconds())) ||
                (relay && sample.has_ipc_write_microseconds()) ||
                (sample.has_ipc_write_microseconds() && sample.ipc_write_microseconds() > kMaximumDuration) ||
                (sample.has_send_queue_wait_microseconds() && sample.send_queue_wait_microseconds() > kMaximumDuration) ||
                (sample.has_native_write_microseconds() && sample.native_write_microseconds() > kMaximumDuration)) {
                ++dropped;
                continue;
            }
            Pending entry{key, now, sample.ByteSizeLong(), worker,
                          sample.has_ipc_write_microseconds() ? std::optional(sample.ipc_write_microseconds()) : std::nullopt,
                          sample.has_send_queue_wait_microseconds() ? std::optional(sample.send_queue_wait_microseconds()) : std::nullopt,
                          sample.has_native_write_microseconds() ? std::optional(sample.native_write_microseconds()) : std::nullopt};
            if (known(key)) {
                observe(entry);
                continue;
            }
            std::array<DesktopFrameKey, 16> keys{};
            std::size_t keyCount = 0, bytes = 0;
            bool sameKey = false;
            for (const auto& pending : pending_) {
                if (!pending) {
                    continue;
                }
                bytes += pending->bytes;
                sameKey |= pending->key == key;
                if (std::find(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(keyCount), pending->key) == keys.begin() + static_cast<std::ptrdiff_t>(keyCount)) {
                    keys[keyCount++] = pending->key;
                }
            }
            auto vacant = std::ranges::find_if(pending_, [](const auto& item) { return !item; });
            if (vacant == pending_.end() || (!sameKey && keyCount == keys.size()) || entry.bytes > 8192 - bytes) {
                ++dropped;
            } else {
                *vacant = entry;
            }
        }
    }
    void expire(Clock::time_point now = Clock::now()) {
        for (auto& entry : pending_) {
            if (entry && now - entry->arrived >= std::chrono::seconds(2)) {
                entry.reset();
                ++dropped;
            }
        }
    }
    std::size_t pendingSamples() const {
        return std::ranges::count_if(pending_, [](const auto& item) { return item.has_value(); });
    }
    // 256 recent local durations per stage, independent of the legacy encoding
    // window. Fixed storage keeps per-session telemetry small and allocation-free.
    std::array<DesktopDurationWindow<256>, static_cast<std::size_t>(DesktopStage::kCount)> stages;
    std::uint64_t dropped = 0;

  private:
    struct Pending {
        DesktopFrameKey key;
        Clock::time_point arrived;
        std::size_t bytes;
        bool worker;
        std::optional<std::uint32_t> ipc, queue, native;
    };
    void observe(const Pending& entry) {
        if (entry.worker && entry.ipc) {
            observe(DesktopStage::kWorkerIpcWrite, *entry.ipc);
        }
        if (!entry.worker && entry.queue) {
            observe(DesktopStage::kRelaySendQueueWait, *entry.queue);
        }
        if (!entry.worker && entry.native) {
            observe(DesktopStage::kRelayNativeWrite, *entry.native);
        }
    }
    std::array<DesktopFrameKey, 128> known_{};
    std::size_t nextKnown_ = 0;
    std::uint64_t generation_ = 0;
    std::array<std::optional<Pending>, 32> pending_{};
    std::optional<Clock::time_point> lastReport_;
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
    DesktopPipelineDiagnostics pipeline;
    DesktopVisibleFrameDiagnostics visibleFrames;
    std::uint32_t videoWidth = 0;
    std::uint32_t videoHeight = 0;
    std::uint32_t latestKeyFrameEncodeMicroseconds = 0;
    std::uint32_t latestTargetFramesPerSecond = 0;
    std::uint32_t latestTargetBitrate = 0;
    bool latestHardwareEncoding = false;
    bool latestGpuEncoding = false;
    std::uint64_t refinementMessages = 0, refinementBytes = 0;
    std::uint64_t regionMessages = 0, regionBytes = 0, copiedPixels = 0, tileCacheHits = 0;
    std::uint64_t videoGeneration = 0, latestVideoFrame = 0;
    std::chrono::steady_clock::time_point renderReportedAt{};
};
} // namespace ultraterimal::server
