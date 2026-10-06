#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ultraterimal::test::cadence {
// Benchmark-only: 60 seconds at 60 FPS fits without reservoir sampling.
inline constexpr std::size_t kSampleCapacity = 16384;
class Samples {
  public:
    explicit Samples(std::size_t capacity = kSampleCapacity) : capacity_(capacity) {
        values_.reserve(capacity);
    }
    void observe(double value) {
        ++count_;
        total_ += value;
        peak_ = std::max(peak_, value);
        ge100ms_ += value >= 100000;
        if (values_.size() < capacity_) {
            values_.push_back(value);
            sorted_ = false;
        } else {
            ++dropped_;
        }
    }
    std::uint64_t count() const { return count_; }
    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t ge100ms() const { return ge100ms_; }
    std::optional<double> mean() const { return count_ ? std::optional(total_ / count_) : std::nullopt; }
    std::optional<double> peak() const { return count_ ? std::optional(peak_) : std::nullopt; }
    std::optional<double> percentile(unsigned percent) {
        if (!count_ || dropped_ || percent == 0 || percent > 100) {
            return std::nullopt; // Never present a truncated prefix as the exact tail.
        }
        if (!sorted_) {
            std::sort(values_.begin(), values_.end());
            sorted_ = true;
        }
        return values_[(values_.size() * percent + 99) / 100 - 1];
    }

  private:
    std::size_t capacity_;
    std::vector<double> values_;
    std::uint64_t count_ = 0, dropped_ = 0, ge100ms_ = 0;
    double total_ = 0, peak_ = 0;
    bool sorted_ = false;
};
inline void number(std::ostream& out, std::optional<double> value) {
    if (value) {
        out << *value;
    } else {
        out << "null";
    }
}
// Counts/bytes/grid cardinalities must never pass through floating point.
template <std::unsigned_integral Integer>
inline void integer(std::ostream& out, std::optional<Integer> value) {
    if (value) {
        out << +*value; // Integral promotion also prints byte-sized counts as numbers.
    } else {
        out << "null";
    }
}
// A counter reset is unknown, not unsigned wraparound. Snapshots never use this.
inline std::optional<std::uint64_t> counterDelta(std::uint64_t before, std::uint64_t after) {
    return after >= before ? std::optional(after - before) : std::nullopt;
}
// IDs are worker-local. Only a new association first seen in a wholly measured
// dispatch (or that same already-qualified ID) can qualify a retained snapshot.
inline bool qualifiedAssociation(std::uint64_t id, std::uint64_t beforeId, std::optional<std::uint64_t> qualifiedId, bool measuredDispatch) {
    return measuredDispatch && id != 0 && (id > beforeId || qualifiedId == id);
}
inline std::size_t boundedCheckpointCount(std::uint32_t count) {
    return std::min<std::size_t>(count, 32);
}
struct PaintRead {
    std::optional<std::uint64_t> count, qpcBefore, qpcAfter;
    std::uint64_t windowBeforeNanoseconds = 0, windowAfterNanoseconds = 0;
};
// Two endpoint reads only; callback entry counts are not RGB changes or presents.
struct PaintMeasurement {
    std::optional<std::uint64_t> frequency;
    std::optional<PaintRead> begin, end;
    bool observe(PaintRead sample, std::int64_t beforeNanoseconds, std::int64_t afterNanoseconds, std::int64_t windowNanoseconds) {
        if (windowNanoseconds <= 0 || beforeNanoseconds < 0 || afterNanoseconds < beforeNanoseconds || afterNanoseconds >= windowNanoseconds) {
            return false; // Never relabel warmup or a final-next/boundary read.
        }
        sample.windowBeforeNanoseconds = static_cast<std::uint64_t>(beforeNanoseconds);
        sample.windowAfterNanoseconds = static_cast<std::uint64_t>(afterNanoseconds);
        if (!begin) {
            begin = sample;
        }
        end = sample;
        return true;
    }
    bool qualified() const {
        const auto valid = [](const auto& sample) {
            return sample && sample->count && sample->qpcBefore && sample->qpcAfter && *sample->qpcBefore <= *sample->qpcAfter;
        };
        return frequency && *frequency > 0 && valid(begin) && valid(end) && *end->count >= *begin->count &&
               *end->qpcBefore >= *begin->qpcAfter && *end->qpcAfter > *begin->qpcBefore && end->windowBeforeNanoseconds >= begin->windowAfterNanoseconds;
    }
    std::optional<std::uint64_t> countDelta() const { return qualified() ? counterDelta(*begin->count, *end->count) : std::nullopt; }
    std::optional<std::uint64_t> minimumTicks() const { return qualified() ? std::optional(*end->qpcBefore - *begin->qpcAfter) : std::nullopt; }
    std::optional<std::uint64_t> maximumTicks() const { return qualified() ? std::optional(*end->qpcAfter - *begin->qpcBefore) : std::nullopt; }
    std::optional<double> rate(bool upper) const {
        const auto ticks = upper ? minimumTicks() : maximumTicks();
        return ticks && *ticks ? std::optional(static_cast<double>(*countDelta()) * static_cast<double>(*frequency) / static_cast<double>(*ticks)) : std::nullopt;
    }
    std::optional<double> estimatedRate() const {
        if (!qualified()) {
            return std::nullopt;
        }
        const auto midpointTicks = (static_cast<double>(*minimumTicks()) + static_cast<double>(*maximumTicks())) / 2;
        return static_cast<double>(*countDelta()) * static_cast<double>(*frequency) / midpointTicks;
    }
    void write(std::ostream& out) const {
        const auto sample = [&](const std::optional<PaintRead>& value) {
            if (!value) {
                out << "null";
                return;
            }
            out << "{\"callback_count\":";
            integer(out, value->count);
            out << ",\"qpc_before_read\":";
            integer(out, value->qpcBefore);
            out << ",\"qpc_after_read\":";
            integer(out, value->qpcAfter);
            out << ",\"window_before_ns\":" << value->windowBeforeNanoseconds << ",\"window_after_ns\":" << value->windowAfterNanoseconds << '}';
        };
        out << ",\"actual_wm_paint_measurement\":{\"count_scope\":\"callback_entries_between_sampled_reads;not_rgb_change_or_dxgi_present\",\"rate_scope\":\"qpc_bracket_bounds_and_midpoint_estimate;not_complete_window_trace\",\"qualified\":" << (qualified() ? "true" : "false") << ",\"reason\":" << (qualified() ? "null" : "\"unknown\"") << ",\"qpc_frequency\":";
        integer(out, frequency);
        out << ",\"begin\":";
        sample(begin);
        out << ",\"end\":";
        sample(end);
        out << ",\"callback_count_delta\":";
        integer(out, countDelta());
        out << ",\"interval_min_qpc_ticks\":";
        integer(out, minimumTicks());
        out << ",\"interval_max_qpc_ticks\":";
        integer(out, maximumTicks());
        out << ",\"callback_hz_lower\":";
        number(out, rate(false));
        out << ",\"callback_hz_upper\":";
        number(out, rate(true));
        out << ",\"callback_hz_estimate\":";
        number(out, estimatedRate());
        out << ",\"read_policy\":\"seq_cst_counter;non_atomic_qpc_brackets;only_reads_wholly_inside_measurement_window\"}";
    }
};
using Timing = std::array<std::optional<std::uint32_t>, 9>;
struct Stages {
    std::array<Samples, 9> samples;
    // VideoFrame and DYNAMIC_DETAIL from one update share timing. Prefer the
    // primary value; use the companion only for stages absent in the primary.
    void observe(const Timing& primary, const Timing& companion = {}) {
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto value = primary[index] ? primary[index] : companion[index];
            if (value) {
                samples[index].observe(*value);
            }
        }
    }
    void write(std::ostream& out, std::string_view prefix = "") {
        constexpr std::array names{"worker_queue", "acquire_wait", "source_age", "pacing_credit_wait", "gpu_sync_wait", "readback_ready", "prepare_cpu", "png_encode", "video_encode"};
        for (std::size_t index = 0; index < samples.size(); ++index) {
            auto& sample = samples[index];
            out << ",\"" << prefix << names[index] << "_samples\":" << sample.count();
            const auto field = [&](std::string_view suffix, std::optional<double> value) {
                out << ",\"" << prefix << names[index] << suffix << "\":";
                number(out, value);
            };
            field("_mean_us", sample.mean());
            field("_p95_us", sample.percentile(95));
            field("_p99_us", sample.percentile(99));
            field("_peak_us", sample.peak());
            out << ",\"" << prefix << names[index] << "_ge_100ms\":" << sample.ge100ms()
                << ",\"" << prefix << names[index] << "_drop\":" << sample.dropped();
        }
    }
};
// Generation/frame are part of the key: patches on different baselines do not
// establish >32-tile static coverage on a single composed canvas.
class Coverage {
  public:
    explicit Coverage(std::size_t capacity = 65536) : capacity_(capacity) {}
    void observe(std::uint64_t generation, std::uint64_t frame, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height) {
        if (!width || !height || overflow_) {
            return;
        }
        const auto right = static_cast<std::uint64_t>(x) + width - 1;
        const auto bottom = static_cast<std::uint64_t>(y) + height - 1;
        for (auto row = static_cast<std::uint64_t>(y) / 64; row <= bottom / 64; ++row) {
            for (auto column = static_cast<std::uint64_t>(x) / 128; column <= right / 128; ++column) {
                const Key key{generation, frame, column, row};
                if (!cells_.contains(key)) {
                    if (cells_.size() == capacity_) {
                        overflow_ = true;
                        return;
                    }
                    cells_.insert(key);
                }
            }
        }
    }
    bool overflow() const { return overflow_; }
    std::optional<std::size_t> unique() const { return overflow_ ? std::nullopt : std::optional(cells_.size()); }
    std::optional<std::size_t> maximumPerBase() const {
        if (overflow_) {
            return std::nullopt;
        }
        std::size_t peak = 0, count = 0;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> previous;
        for (const auto& key : cells_) {
            const auto base = std::pair{std::get<0>(key), std::get<1>(key)};
            count = previous == base ? count + 1 : 1;
            previous = base;
            peak = std::max(peak, count);
        }
        return peak;
    }

  private:
    using Key = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>;
    std::size_t capacity_;
    std::set<Key> cells_;
    bool overflow_ = false;
};
} // namespace ultraterimal::test::cadence
