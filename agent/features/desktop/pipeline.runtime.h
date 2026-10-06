#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include "ultraterimal/protocol/message.protocol.h"

namespace ultraterimal::agent {
// Best effort, owner-loop diagnostics. No timers, ACKs or recursive samples.
// Worker and relay flush on existing traffic events, at most twice per second.
class DesktopPipelineBuffer {
  public:
    using Clock = std::chrono::steady_clock;
    static std::uint32_t duration(Clock::duration elapsed) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 0, 60000000));
    }
    static std::optional<wire::DesktopPipelineSample> sample(const wire::Envelope& message, wire::DesktopPipelineSample::Source source) {
        if (!message.has_video_frame() && !message.has_desktop_region_update() && !message.has_video_refinement()) {
            return std::nullopt;
        }
        wire::DesktopPipelineSample result;
        result.set_generation(message.generation());
        result.set_frame_id(message.frame_id());
        result.set_batch_id(message.has_video_refinement() ? message.video_refinement().batch_id() : 0);
        result.set_source(source);
        return result;
    }
    void add(wire::DesktopPipelineSample sample) {
        if (report_.samples_size() == 32) {
            drop(1);
        } else {
            *report_.add_samples() = std::move(sample);
        }
    }
    void merge(const wire::DesktopPipelineReport& report) {
        drop(report.dropped_samples());
        for (const auto& sample : report.samples()) {
            add(sample);
        }
    }
    std::optional<wire::Envelope> pending(Clock::time_point now) const {
        if (now < nextReport_ || (!report_.samples_size() && !report_.dropped_samples())) {
            return std::nullopt;
        }
        wire::Envelope message;
        message.set_channel_id("input");
        *message.mutable_desktop_pipeline_report() = report_;
        return message;
    }
    void sent(Clock::time_point now) {
        report_.Clear();
        nextReport_ = now + std::chrono::milliseconds(500);
    }
    void clear() { report_.Clear(); }

  private:
    void drop(std::uint32_t count) {
        report_.set_dropped_samples(report_.dropped_samples() + std::min(count, std::numeric_limits<std::uint32_t>::max() - report_.dropped_samples()));
    }
    wire::DesktopPipelineReport report_;
    Clock::time_point nextReport_{};
};
} // namespace ultraterimal::agent
