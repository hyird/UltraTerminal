#include <algorithm>
#include <array>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <psapi.h>
#include <ruvia/core/asio_task.h>
#include <ruvia/core/execution_domain.h>

#include "capture.fixture.h"
#include "config/version.config.h"
#include "features/desktop/video.runtime.h"
#include "video_cadence_benchmark.samples.h"
#include "video_cadence_benchmark.surface.h"

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace cadence = ultraterimal::test::cadence;
void jsonString(std::ostream& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            out << '\\' << character;
        } else if (character < 32) {
            out << "\\u00" << hex[character / 16] << hex[character % 16];
        } else {
            out << character;
        }
    }
    out << '"';
}
cadence::Timing timingValues(const ultraterimal::wire::DesktopStageTiming& timing) {
    const std::array present{timing.has_worker_queue_microseconds(), timing.has_acquire_wait_microseconds(), timing.has_source_age_microseconds(), timing.has_pacing_credit_wait_microseconds(), timing.has_gpu_sync_wait_microseconds(), timing.has_readback_ready_microseconds(), timing.has_prepare_cpu_microseconds(), timing.has_png_encode_microseconds(), timing.has_video_encode_microseconds()};
    const std::array values{timing.worker_queue_microseconds(), timing.acquire_wait_microseconds(), timing.source_age_microseconds(), timing.pacing_credit_wait_microseconds(), timing.gpu_sync_wait_microseconds(), timing.readback_ready_microseconds(), timing.prepare_cpu_microseconds(), timing.png_encode_microseconds(), timing.video_encode_microseconds()};
    cadence::Timing result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        if (present[index]) {
            result[index] = values[index];
        }
    }
    return result;
}
struct BodyBytes {
    std::uint64_t count = 0, payload = 0, serializedBody = 0;
    template <class Message>
    void observe(const Message& message, std::uint64_t payloadBytes = 0) {
        ++count;
        payload += payloadBytes;
        serializedBody += message.ByteSizeLong();
    }
};
struct RefinementMeta {
    std::uint64_t generation = 0, frame = 0, batch = 0, tiles = 0, payload = 0, serializedBody = 0;
    int kind = 0;
    bool measured = false;
    std::uint64_t left = 0, top = 0, right = 0, bottom = 0;
    std::optional<std::size_t> uniqueGrid;
};
struct ConfigMeta {
    ultraterimal::wire::VideoConfig config;
    std::uint64_t generation = 0, frame = 0;
    std::int32_t left = 0, top = 0;
};
struct StageSamples {
    cadence::Stages primary, staticDetail;
    cadence::PaintMeasurement paintMeasurement;
    cadence::Coverage staticCoverage, dynamicCoverage;
    std::array<BodyBytes, 7> bodies;
    BodyBytes initialFeedback, initialCapabilities;
    std::vector<RefinementMeta> refinements;
    std::vector<ConfigMeta> configurations;
    std::uint64_t refinementBatches = 0, refinementTiles = 0, refinementBytes = 0;
    std::uint64_t staticBatches = 0, dynamicBatches = 0, legacyBatches = 0;
    std::uint64_t refinementMetaDrop = 0, configMetaDrop = 0, cadenceDrop = 0, videoUpdates = 0;
    std::uint64_t observedStaticBatches = 0, observedDynamicBatches = 0;
    double measurementWindowSeconds = 0, noPrimaryOutputTailMs = 0;
    std::optional<Clock::time_point> lastPrimaryOutput;
    std::array<std::optional<std::uint64_t>, 10> comparisonDeltas;
    std::optional<ultraterimal::agent::DesktopVideoDiagnostics> measurementSnapshot;
    std::optional<std::uint64_t> qualifiedAdmissionId, qualifiedComparisonId, qualifiedReadbackId;
    bool timerQualified = false;
    std::optional<double> snapshotBeginSeconds, snapshotEndSeconds;
    void observeDiagnostics(const ultraterimal::agent::DesktopVideoDiagnostics& before, const ultraterimal::agent::DesktopVideoDiagnostics& after, Clock::time_point began, Clock::time_point ended, Clock::time_point warmup) {
        const auto id = [](const auto& value) { return value ? value->id : 0; };
        if (after.lastAdmission && cadence::qualifiedAssociation(id(after.lastAdmission), id(before.lastAdmission), qualifiedAdmissionId, true)) {
            qualifiedAdmissionId = after.lastAdmission->id;
        }
        if (after.comparison.lastAdmission && after.lastAdmission && id(after.comparison.lastAdmission) == id(after.lastAdmission) && qualifiedAdmissionId == id(after.lastAdmission)) {
            qualifiedComparisonId = after.comparison.lastAdmission->id;
        }
        if (after.lastReadback && (qualifiedAdmissionId == after.lastReadback->admission.id || qualifiedComparisonId == after.lastReadback->admission.id)) {
            qualifiedReadbackId = after.lastReadback->admission.id;
        }
        timerQualified = after.returnedSnapshots > before.returnedSnapshots;
        measurementSnapshot = after; // One bounded, wholly measured dispatch only.
        snapshotBeginSeconds = std::chrono::duration<double>(began - warmup).count();
        snapshotEndSeconds = std::chrono::duration<double>(ended - warmup).count();
    }
    StageSamples() {
        refinements.reserve(cadence::kSampleCapacity);
        configurations.reserve(128);
    }
    void observe(const ultraterimal::agent::DesktopVideoUpdate& update, bool measured) {
        using namespace ultraterimal;
        const bool hasVideo = !update.frame.data().empty();
        if (update.configuration) {
            if (configurations.size() < 128) {
                configurations.push_back({*update.configuration, update.generation, update.frameId, update.desktopLeft, update.desktopTop});
            } else {
                ++configMetaDrop;
            }
        }
        cadence::Timing primaryTiming{}, dynamicTiming{};
        if (measured) {
            videoUpdates += hasVideo || update.region.has_value();
            if (hasVideo) {
                bodies[0].observe(update.frame, update.frame.data().size());
                primaryTiming = timingValues(update.frame.stage_timing());
            }
            if (update.region) {
                std::uint64_t payload = 0;
                for (const auto& patch : update.region->patches()) {
                    payload += patch.png().size();
                }
                bodies[2].observe(*update.region, payload);
                // Region and video are alternatives in the current runtime.
                // Preserve one sample per update even if both are ever supplied.
                const auto regionTiming = timingValues(update.region->stage_timing());
                for (std::size_t index = 0; index < primaryTiming.size(); ++index) {
                    if (!primaryTiming[index]) {
                        primaryTiming[index] = regionTiming[index];
                    }
                }
            }
            if (update.configuration) {
                bodies[3].observe(*update.configuration, update.configuration->description().size());
            }
            if (update.cursor) {
                bodies[4].observe(*update.cursor, update.cursor->rgba().size());
            }
        }
        if (update.refinement) {
            const auto& refinement = *update.refinement;
            RefinementMeta meta{update.generation, update.frameId, refinement.batch_id(), static_cast<std::uint64_t>(refinement.patches_size()), 0, refinement.ByteSizeLong(), static_cast<int>(refinement.kind()), measured};
            cadence::Coverage batchCoverage;
            bool first = true;
            for (const auto& patch : refinement.patches()) {
                meta.payload += patch.png().size();
                const auto& rect = patch.destination();
                const auto right = static_cast<std::uint64_t>(rect.x()) + rect.width();
                const auto bottom = static_cast<std::uint64_t>(rect.y()) + rect.height();
                meta.left = first ? rect.x() : std::min(meta.left, static_cast<std::uint64_t>(rect.x()));
                meta.top = first ? rect.y() : std::min(meta.top, static_cast<std::uint64_t>(rect.y()));
                meta.right = std::max(meta.right, right);
                meta.bottom = std::max(meta.bottom, bottom);
                first = false;
                batchCoverage.observe(update.generation, update.frameId, rect.x(), rect.y(), rect.width(), rect.height());
                if (refinement.kind() == wire::STATIC_DETAIL) {
                    staticCoverage.observe(update.generation, update.frameId, rect.x(), rect.y(), rect.width(), rect.height());
                } else if (refinement.kind() == wire::DYNAMIC_DETAIL) {
                    dynamicCoverage.observe(update.generation, update.frameId, rect.x(), rect.y(), rect.width(), rect.height());
                }
            }
            meta.uniqueGrid = batchCoverage.unique();
            observedStaticBatches += refinement.kind() == wire::STATIC_DETAIL;
            observedDynamicBatches += refinement.kind() == wire::DYNAMIC_DETAIL;
            if (refinements.size() < cadence::kSampleCapacity) {
                refinements.push_back(meta);
            } else {
                ++refinementMetaDrop;
            }
            if (measured) {
                ++refinementBatches;
                refinementTiles += meta.tiles;
                refinementBytes += meta.serializedBody;
                bodies[1].observe(refinement, meta.payload);
                staticBatches += refinement.kind() == wire::STATIC_DETAIL;
                dynamicBatches += refinement.kind() == wire::DYNAMIC_DETAIL;
                legacyBatches += refinement.kind() == wire::UNSPECIFIED;
                if (refinement.kind() == wire::STATIC_DETAIL) {
                    staticDetail.observe(timingValues(refinement.stage_timing()));
                } else {
                    dynamicTiming = timingValues(refinement.stage_timing());
                }
            }
        }
        if (measured) {
            primary.observe(primaryTiming, dynamicTiming);
        }
    }
};
std::uint64_t cpuTicks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("Cannot measure benchmark CPU time");
    }
    const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
using OptionalDiagnostics = ultraterimal::agent::DesktopOptionalDiagnostics;
constexpr std::array<std::pair<const char*, std::uint64_t OptionalDiagnostics::*>, 16> kOptionalCounters{{
    {"normal", &OptionalDiagnostics::normal},
    {"probe", &OptionalDiagnostics::probe},
    {"fit_denied", &OptionalDiagnostics::fitDenied},
    {"advantage_denied", &OptionalDiagnostics::advantageDenied},
    {"unknown_prediction", &OptionalDiagnostics::unknownPrediction},
    {"cpu_source", &OptionalDiagnostics::cpuSource},
    {"roi_source", &OptionalDiagnostics::roiSource},
    {"actual_png_bytes", &OptionalDiagnostics::actualPngBytes},
    {"known_png_products", &OptionalDiagnostics::knownPngProducts},
    {"unknown_png_products", &OptionalDiagnostics::unknownPngProducts},
    {"reliable_references", &OptionalDiagnostics::reliableReferences},
    {"local_png_reuses", &OptionalDiagnostics::localPngReuses},
    {"materialize_xor_cursor", &OptionalDiagnostics::materializeXorCursor},
    {"materialize_legacy_detail", &OptionalDiagnostics::materializeLegacyDetail},
    {"materialize_software", &OptionalDiagnostics::materializeSoftware},
    {"materialize_gpu_fallback", &OptionalDiagnostics::materializeGpuFallback},
}};
using ComparisonDiagnostics = ultraterimal::agent::DesktopComparisonDiagnostics;
constexpr std::array<std::pair<const char*, std::uint64_t ComparisonDiagnostics::*>, 10> kComparisonCounters{{
    {"baseline_commits", &ComparisonDiagnostics::baselineCommits},
    {"baseline_failures", &ComparisonDiagnostics::baselineFailures},
    {"baseline_cpu_microseconds", &ComparisonDiagnostics::baselineCpuMicroseconds},
    {"baseline_gpu_bytes", &ComparisonDiagnostics::baselineGpuBytes},
    {"submissions", &ComparisonDiagnostics::submissions},
    {"finishes", &ComparisonDiagnostics::finishes},
    {"failures", &ComparisonDiagnostics::failures},
    {"proofs", &ComparisonDiagnostics::proofs},
    {"unchanged", &ComparisonDiagnostics::unchanged},
    {"rejected_proofs", &ComparisonDiagnostics::rejectedProofs},
}};
void writeTimer(std::ostream& out, const ultraterimal::agent::WindowsTimerResolutionStatus& timer) {
    out << std::boolalpha << "{\"requested\":" << timer.requested << ",\"active\":" << timer.active << ",\"period_ms\":" << timer.periodMilliseconds << ",\"caps_result\":";
    cadence::integer(out, timer.capabilitiesResult);
    out << ",\"begin_result\":";
    cadence::integer(out, timer.beginResult);
    out << ",\"end_result\":";
    cadence::integer(out, timer.endResult);
    out << '}';
}
void writeAdmission(std::ostream& out, const ultraterimal::agent::DesktopAdmissionSnapshot& work) {
    const auto us = [](auto value) { return std::chrono::duration_cast<std::chrono::microseconds>(value).count(); };
    const auto optionalUs = [&](const auto& value) {
        if (value) {
            out << us(*value);
        } else {
            out << "null";
        }
    };
    out << "{\"id\":" << work.id << ",\"job_id\":" << work.jobId << ",\"revision\":" << work.revision
        << ",\"source\":\"" << (work.source == ultraterimal::agent::DesktopPreparationSource::kCpu ? "cpu" : "roi")
        << "\",\"capture_epoch\":" << work.captureEpoch << ",\"source_serial\":" << work.sourceSerial << ",\"source_generation\":" << work.sourceGeneration
        << ",\"composed_base\":" << work.composedBase << ",\"width\":" << work.width << ",\"height\":" << work.height << ",\"rotation\":" << work.rotation
        << ",\"desktop_left\":" << work.desktopLeft << ",\"desktop_top\":" << work.desktopTop
        << ",\"fps\":" << work.framesPerSecond << ",\"bitrate\":" << work.bitrate << ",\"work_units\":" << work.workUnits
        << ",\"allowance_us\":" << work.allowance.count() << ",\"window_us\":" << work.window.count() << ",\"remaining_at_admission_us\":" << work.remainingAtAdmission.count()
        << ",\"started_local_us\":" << us(work.started) << ",\"admitted_at_local_us\":" << us(work.admittedAt)
        << ",\"encode_at_local_us\":" << us(work.encodeAt) << ",\"deadline_local_us\":" << us(work.deadline)
        << ",\"decision_deadline_local_us\":";
    optionalUs(work.decisionDeadline);
    out << ",\"shared_probe_before_local_us\":";
    optionalUs(work.sharedProbeBefore);
    out << ",\"prepare_prediction_us\":";
    optionalUs(work.preparePrediction);
    out << ",\"send_prediction_us\":";
    optionalUs(work.sendPrediction);
    out << ",\"message_bytes\":";
    cadence::integer(out, work.messageBytes);
    out << ",\"video_encode_estimate_us\":" << work.videoEncodeEstimate.count() << ",\"video_alternative_us\":" << work.videoAlternative.count()
        << ",\"preparation_prefix_us\":" << work.preparationPrefix.count() << ",\"video_bytes\":" << work.videoBytes
        << ",\"probe_reasons\":" << work.probeReasons << ",\"choice\":" << static_cast<int>(work.choice)
        << ",\"comparison\":" << work.comparison << ",\"region\":" << work.region << ",\"fit_denied\":" << work.fitDenied
        << ",\"advantage_denied\":" << work.advantageDenied << ",\"unknown_prediction\":" << work.unknownPrediction << '}';
}
void writeDiagnosticEvidence(std::ostream& out, const StageSamples& stages) {
    out << std::boolalpha << ",\"comparison_counter_delta_scope\":\"existing_counter_baseline_to_final_next;null_on_reset_or_no_baseline\",\"comparison_counter_deltas\":{";
    for (std::size_t index = 0; index < kComparisonCounters.size(); ++index) {
        if (index) {
            out << ',';
        }
        jsonString(out, kComparisonCounters[index].first);
        out << ':';
        cadence::integer(out, stages.comparisonDeltas[index]);
    }
    out << "},\"local_diagnostic_metadata\":{\"scope\":\"last_wholly_measured_dispatch;not_counters\",\"qualification_policy\":\"fresh_admission_in_measured_dispatch_or_same_qualified_id;not_entire_work_duration\",\"time_scope\":\"worker_local_epoch_not_wire_or_qpc\",\"duration_scope\":\"wall_or_cpu_evidence_not_pure_gpu_execution\",\"checkpoint_capacity\":32,\"snapshot_dispatch_begin_seconds\":";
    cadence::number(out, stages.snapshotBeginSeconds);
    out << ",\"snapshot_dispatch_end_seconds\":";
    cadence::number(out, stages.snapshotEndSeconds);
    const auto& snapshot = stages.measurementSnapshot;
    const auto envelope = [&](const char* name, bool qualified) {
        out << ",\"" << name << "\":{\"qualified\":" << qualified << ",\"reason\":" << (qualified ? "null" : "\"unknown\"") << ",\"snapshot\":";
    };
    envelope("worker_timer", snapshot && stages.timerQualified);
    if (snapshot && stages.timerQualified) {
        writeTimer(out, snapshot->timerResolution);
    } else {
        out << "null";
    }
    out << '}';
    const bool admissionQualified = snapshot && snapshot->lastAdmission && stages.qualifiedAdmissionId == snapshot->lastAdmission->id;
    envelope("work_admission", admissionQualified);
    if (admissionQualified) {
        writeAdmission(out, *snapshot->lastAdmission);
    } else {
        out << "null";
    }
    out << '}';
    const bool comparisonQualified = snapshot && snapshot->comparison.lastAdmission && stages.qualifiedComparisonId == snapshot->comparison.lastAdmission->id;
    envelope("comparison_admission", comparisonQualified);
    if (comparisonQualified) {
        writeAdmission(out, *snapshot->comparison.lastAdmission);
        // lastSpent/lastPixels/lastBaseline* have no owned work association in
        // the public header. A fresh admission cannot qualify those old values.
    } else {
        out << "null";
    }
    out << '}';
    envelope("comparison_last_observation", false);
    out << "null}";
    const bool readbackQualified = snapshot && snapshot->lastReadback && stages.qualifiedReadbackId == snapshot->lastReadback->admission.id;
    envelope("readback_work", readbackQualified);
    if (readbackQualified) {
        const auto& trace = *snapshot->lastReadback;
        out << "{\"admission\":";
        writeAdmission(out, trace.admission);
        out << ",\"detail_work\":";
        if (trace.detailWork) {
            out << "{\"generation\":" << trace.detailWork->generation << ",\"frame\":" << trace.detailWork->frame << ",\"slice\":" << trace.detailWork->slice << '}';
        } else {
            out << "null";
        }
        out << ",\"purpose\":" << trace.purpose << ",\"submitted_tiles\":" << trace.submittedTiles << ",\"submitted_work_units\":" << trace.submittedWorkUnits
            << ",\"submitted_pixels\":" << trace.submittedPixels << ",\"retirement_reason\":" << static_cast<int>(trace.retirementReason)
            << ",\"video_fallback_reason\":" << static_cast<int>(trace.videoFallbackReason) << ",\"fallback_job_id\":" << trace.fallbackJobId
            << ",\"fallback_source_serial\":" << trace.fallbackSourceSerial << ",\"checkpoint_count\":" << trace.count << ",\"checkpoint_dropped\":" << trace.dropped
            << ",\"checkpoint_clamped\":" << (trace.count > trace.checkpoints.size()) << ",\"checkpoints\":[";
        for (std::size_t index = 0; index < cadence::boundedCheckpointCount(trace.count); ++index) {
            const auto& point = trace.checkpoints[index];
            if (index) {
                out << ',';
            }
            out << "{\"kind\":" << static_cast<int>(point.kind) << ",\"job_id\":" << point.jobId << ",\"revision\":" << point.revision
                << ",\"capture_epoch\":" << point.captureEpoch << ",\"source_serial\":" << point.sourceSerial << ",\"generation\":" << point.generation << ",\"composed_base\":" << point.composedBase
                << ",\"at_local_us\":" << std::chrono::duration_cast<std::chrono::microseconds>(point.at).count()
                << ",\"remaining_us\":" << point.remaining.count() << ",\"cpu_minimum_us\":" << point.cpuMinimum.count() << ",\"overlapping_work_us\":" << point.overlappingWork.count()
                << ",\"observation_work_units\":" << point.observationWorkUnits << ",\"at_or_after_deadline\":" << point.atOrAfterDeadline
                << ",\"physical_needs_drain\":" << point.physicalNeedsDrain << ",\"ready_owned\":" << point.readyOwned << ",\"timer\":";
            writeTimer(out, point.timer);
            out << ",\"status\":";
            cadence::integer(out, point.status);
            out << ",\"error\":";
            if (point.error) {
                out << *point.error;
            } else {
                out << "null";
            }
            out << ",\"finished_prepare_prediction_us\":";
            if (point.finishedPreparePrediction) {
                out << point.finishedPreparePrediction->count();
            } else {
                out << "null";
            }
            out << '}';
        }
        out << "]}";
    } else {
        out << "null";
    }
    out << "}}";
}
asio::awaitable<void> measure(ultraterimal::agent::DesktopVideoRuntime& video, std::chrono::seconds duration, bool sharp, bool regions, bool optimization, unsigned& regionFrames, std::vector<Clock::time_point>& received, std::vector<double>& sourceAges, std::vector<std::uint64_t>& presentTicks, std::vector<std::uint64_t>& encodeStartedUs, std::int64_t qpcFrequency, unsigned& sourceWidth, unsigned& sourceHeight, unsigned& lastTargetRate,
                              unsigned& hardwareFrames, unsigned& gpuFrames, std::uint64_t& totalEncodeMicroseconds, std::uint64_t& totalEncodedBytes, unsigned& calls, unsigned& noUpdate, unsigned& cursorOnly,
                              ultraterimal::agent::DesktopVideoDiagnostics& captured, StageSamples& stages, const cadence::PaintCallbackCounter& paintCounter) {
    using namespace ultraterimal;
    std::optional<agent::DesktopVideoUpdate> first;
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline && (!first || first->frame.data().empty())) {
        first = co_await video.next();
    }
    if (!first || first->frame.data().empty() || !first->configuration) {
        throw std::runtime_error("Cannot establish desktop video baseline and geometry");
    }
    stages.observe(*first, false);
    stages.lastPrimaryOutput = Clock::now();
    sourceWidth = first->configuration->width();
    sourceHeight = first->configuration->height();
    wire::QualityFeedback feedback;
    feedback.set_preference(sharp ? wire::QualityFeedback::SHARP : wire::QualityFeedback::SMOOTH);
    feedback.set_video_refinement(optimization);
    feedback.set_displayed_generation(first->generation);
    feedback.set_displayed_frame(first->frameId);
    video.feedback(feedback);
    stages.initialFeedback.observe(feedback);
    wire::DesktopVideoCapabilities capabilities;
    capabilities.add_codecs(wire::AVC);
    capabilities.set_region_updates(regions);
    capabilities.set_tile_cache(regions);
    capabilities.set_desktop_optimization_contract(optimization ? 2 : 0);
    video.capabilities(capabilities);
    stages.initialCapabilities.observe(capabilities);
    const auto until = Clock::now() + 3s + duration;
    const auto warmup = until - duration;
    agent::DesktopVideoDiagnostics baseline;
    bool sampling = false;
    stages.paintMeasurement.frequency = cadence::paintQpcFrequency();
    while (Clock::now() < until) {
        const auto before = video.diagnostics();
        const auto dispatchBegan = Clock::now();
        auto update = co_await video.next();
        const auto now = Clock::now();
        if (now >= warmup && !sampling) {
            baseline = before;
            sampling = true;
        }
        if (now >= warmup && now < until) {
            ++calls;
            if (!update) {
                ++noUpdate;
            } else if (update->frame.data().empty() && !update->region && !update->refinement) {
                ++cursorOnly;
            }
        }
        const bool measured = now >= warmup && now < until;
        if (measured && dispatchBegan >= warmup) {
            stages.observeDiagnostics(before, video.diagnostics(), dispatchBegan, now, warmup);
        }
        if (measured) {
            const auto readBefore = Clock::now();
            const auto sample = cadence::readPaintCounter(paintCounter);
            const auto readAfter = Clock::now();
            stages.paintMeasurement.observe(sample, std::chrono::duration_cast<std::chrono::nanoseconds>(readBefore - warmup).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(readAfter - warmup).count(), std::chrono::duration_cast<std::chrono::nanoseconds>(until - warmup).count());
        }
        if (update) {
            stages.observe(*update, measured);
        }
        if (update && update->refinement && update->refinement->batch_id()) {
            wire::Envelope ack;
            ack.set_generation(update->generation);
            ack.set_frame_id(update->frameId);
            ack.mutable_desktop_refinement_ack()->set_batch_id(update->refinement->batch_id());
            ack.mutable_desktop_refinement_ack()->set_status(wire::DesktopRefinementAck::APPLIED);
            video.refinementAck(ack); // Local software commit model, NOT network/display.
            if (measured) {
                stages.bodies[6].observe(ack.desktop_refinement_ack());
            }
        }
        if (!update || (update->frame.data().empty() && !update->region)) {
            continue;
        }
        if (now < until) {
            stages.lastPrimaryOutput = now;
        }
        feedback.set_displayed_generation(update->generation);
        feedback.set_displayed_frame(update->frameId);
        feedback.set_decode_microseconds(1000);
        feedback.set_queue_depth(0);
        video.feedback(feedback);
        if (measured) {
            stages.bodies[5].observe(feedback);
            const bool retain = received.size() < cadence::kSampleCapacity;
            if (retain) {
                received.push_back(now);
            } else {
                ++stages.cadenceDrop;
            }
            if (retain && update->sourcePresentTicks) {
                LARGE_INTEGER published{};
                if (!QueryPerformanceCounter(&published) || static_cast<std::uint64_t>(published.QuadPart) < update->sourcePresentTicks) {
                    throw std::runtime_error("Invalid DXGI present timestamp in video runtime");
                }
                sourceAges.push_back(1000.0 * (static_cast<std::uint64_t>(published.QuadPart) - update->sourcePresentTicks) / qpcFrequency);
                presentTicks.push_back(update->sourcePresentTicks);
                const auto encodeUs = update->region ? update->region->encode_microseconds() : update->frame.encode_microseconds();
                encodeStartedUs.push_back(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()) - encodeUs);
            }
            lastTargetRate = update->region ? update->region->target_frames_per_second() : update->frame.target_frames_per_second();
            regionFrames += update->region.has_value();
            hardwareFrames += update->frame.hardware_encoding();
            gpuFrames += update->frame.gpu_encoding();
            totalEncodeMicroseconds += update->region ? update->region->encode_microseconds() : update->frame.encode_microseconds();
            totalEncodedBytes += update->region ? update->region->ByteSizeLong() : update->frame.ByteSizeLong();
        }
    }
    stages.measurementWindowSeconds = std::chrono::duration<double>(until - warmup).count();
    stages.noPrimaryOutputTailMs = stages.lastPrimaryOutput ? std::max(0.0, std::chrono::duration<double, std::milli>(until - *stages.lastPrimaryOutput).count()) : 0;
    const auto after = video.diagnostics();
    if (sampling) {
        for (std::size_t index = 0; index < kComparisonCounters.size(); ++index) {
            const auto field = kComparisonCounters[index].second;
            stages.comparisonDeltas[index] = cadence::counterDelta(baseline.comparison.*field, after.comparison.*field);
        }
    }
    // Select the existing measurement counters by name, never struct position.
    // Unreported timer/job state would be after-only snapshots, not deltas.
    // Admission/batch associations also need fresh measured work; a retained
    // warmup association must not be relabeled as a new measurement batch.
    captured = {};
    captured.captureCalls = after.captureCalls - baseline.captureCalls;
    captured.pixelUpdates = after.pixelUpdates - baseline.pixelUpdates;
    captured.cursorOnlyUpdates = after.cursorOnlyUpdates - baseline.cursorOnlyUpdates;
    captured.captureTimeouts = after.captureTimeouts - baseline.captureTimeouts;
    captured.pendingBeforeDeadlineCalls = after.pendingBeforeDeadlineCalls - baseline.pendingBeforeDeadlineCalls;
    captured.backpressuredCalls = after.backpressuredCalls - baseline.backpressuredCalls;
    captured.encodedFrames = after.encodedFrames - baseline.encodedFrames;
    captured.pixelCaptureGaps = after.pixelCaptureGaps - baseline.pixelCaptureGaps;
    captured.pixelCaptureGapsGe45ms = after.pixelCaptureGapsGe45ms - baseline.pixelCaptureGapsGe45ms;
    captured.unknownDamageUpdates = after.unknownDamageUpdates - baseline.unknownDamageUpdates;
    captured.regionCandidates = after.regionCandidates - baseline.regionCandidates;
    captured.regionBudgetFailures = after.regionBudgetFailures - baseline.regionBudgetFailures;
    captured.regionTimeBudgetFailures = after.regionTimeBudgetFailures - baseline.regionTimeBudgetFailures;
    captured.filteredPixels = after.filteredPixels - baseline.filteredPixels;
    captured.unchangedPixelUpdates = after.unchangedPixelUpdates - baseline.unchangedPixelUpdates;
    captured.sparseReadbackPixels = after.sparseReadbackPixels - baseline.sparseReadbackPixels;
    captured.refinementBatches = after.refinementBatches - baseline.refinementBatches;
    captured.refinementTiles = after.refinementTiles - baseline.refinementTiles;
    captured.dynamicRefinementBatches = after.dynamicRefinementBatches - baseline.dynamicRefinementBatches;
    captured.roiSubmissions = after.roiSubmissions - baseline.roiSubmissions;
    captured.roiPending = after.roiPending - baseline.roiPending;
    captured.roiBusy = after.roiBusy - baseline.roiBusy;
    captured.roiRetired = after.roiRetired - baseline.roiRetired;
    captured.roiFailures = after.roiFailures - baseline.roiFailures;
    captured.optionalCostDenials = after.optionalCostDenials - baseline.optionalCostDenials;
    captured.optional = after.optional;
    for (const auto& field : kOptionalCounters) {
        captured.optional.*(field.second) -= baseline.optional.*(field.second);
    }
    if (!captured.optional.cpuSource && !captured.optional.roiSource) {
        captured.optional.predictedMessageBytes.reset();
        captured.optional.predictedPrepareMicroseconds.reset();
    }
    if (!captured.optional.knownPngProducts && !captured.optional.unknownPngProducts) {
        captured.optional.lastPngBytes.reset();
    }
    video.stop();
}
} // namespace
int main(int argc, char** argv) {
    try {
        unsigned paintHz = 40;
        int paintSize = 64;
        auto duration = 10s;
        bool sharp = false;
        bool scrolling = false;
        bool motion = false;
        bool regions = false, optimization = false, detail = false, stationary = false, fullDesktop = false, isolatedDesktop = false;
        unsigned regionFrames = 0;
        for (int index = 1; index < argc; ++index) {
            const auto option = std::string_view(argv[index]);
            if (option == "--paint-hz=30") {
                paintHz = 30;
            } else if (option == "--paint-hz=40") {
                paintHz = 40;
            } else if (option == "--paint-hz=60") {
                paintHz = 60;
            } else if (option == "--seconds=60") {
                duration = 60s;
            } else if (option == "--paint-size=512") {
                paintSize = 512;
            } else if (option == "--paint-size=960") {
                paintSize = 960;
            } else if (option == "--quality=sharp") {
                sharp = true;
            } else if (option == "--scroll") {
                scrolling = true;
            } else if (option == "--motion") {
                motion = true;
            } else if (option == "--optimization=v2") {
                optimization = true;
            } else if (option == "--detail") {
                detail = true;
            } else if (option == "--static") {
                stationary = true;
            } else if (option == "--full-desktop") {
                fullDesktop = true;
            } else if (option == "--isolated-desktop") {
                isolatedDesktop = true;
            } else if (option == "--regions") {
                regions = true;
            } else {
                throw std::invalid_argument("Usage: agent-video-cadence-benchmark [--paint-hz=30|40|60] [--paint-size=512|960] [--seconds=60] [--quality=sharp] [--scroll|--motion|--detail] [--static --detail [--full-desktop]] [--isolated-desktop] [--regions] [--optimization=v2]");
            }
        }
        if (static_cast<unsigned>(scrolling) + static_cast<unsigned>(motion) + static_cast<unsigned>(detail) > 1) {
            throw std::invalid_argument("Scroll, motion and detail are mutually exclusive");
        }
        if ((stationary && !detail) || (fullDesktop && !stationary)) {
            throw std::invalid_argument("--static requires --detail; --full-desktop requires --static --detail");
        }
        const auto paintInterval = std::chrono::microseconds(1000000 / paintHz);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        unsigned paints = 0;
        cadence::SurfaceEvidence surfaceEvidence;
        cadence::PaintCallbackCounter paintCounter; // Outlives painter and all coroutine reads.
        std::promise<void> staticReady;
        auto staticReadyFuture = staticReady.get_future();
        std::exception_ptr painterFailure;
        std::jthread painter([&](std::stop_token stop) {
            try {
                if (stationary) {
                    cadence::StaticDetailSurface surface(paintSize, fullDesktop, surfaceEvidence);
                    cadence::PaintCallbackObserver paintObserver(L"UltraterimalCadenceStaticDetailTest", paintCounter);
                    surface.paintOnce();
                    ++paints;
                    staticReady.set_value();
                    while (!stop.stop_requested()) {
                        surface.pump();
                        std::this_thread::sleep_for(10ms);
                    }
                    return;
                }
                cadence::SurfaceEvidence backdropEvidence;
                std::unique_ptr<cadence::StaticDetailSurface> backdrop;
                if (isolatedDesktop) {
                    backdrop = std::make_unique<cadence::StaticDetailSurface>(paintSize, true, backdropEvidence);
                    backdrop->paintOnce();
                }
                ultraterimal::test::TestSurface surface(paintSize);
                cadence::PaintCallbackObserver paintObserver(L"UltraterimalCaptureTest", paintCounter);
                surfaceEvidence.desktop = cadence::displayBounds();
                const auto began = Clock::now();
                while (!stop.stop_requested() && paints < paintHz * (duration.count() + 5)) {
                    std::this_thread::sleep_until(began + paintInterval * paints);
                    if (stop.stop_requested()) {
                        break;
                    }
                    const auto color = paints % 2 ? RGB(208, 32, 32) : RGB(32, 208, 32);
                    if (scrolling) {
                        surface.scroll(color);
                    } else if (motion) {
                        surface.paintMotion(paints);
                    } else if (detail) {
                        surface.paintDetail(paints);
                    } else {
                        surface.paint(color);
                    }
                    ++paints;
                    cadence::inspectSurface(FindWindowW(L"UltraterimalCaptureTest", nullptr), surfaceEvidence);
                }
            } catch (...) {
                painterFailure = std::current_exception();
                if (stationary && paints == 0) {
                    staticReady.set_exception(painterFailure);
                }
            }
        });
        if (stationary) {
            staticReadyFuture.get();
        }
        asio::io_context io;
        ruvia::execution_domain attachment(io);
        ultraterimal::agent::DesktopVideoRuntime video(attachment.handle(), 0, optimization ? 2 : 0);
        std::vector<Clock::time_point> received;
        std::vector<double> sourceAges;
        std::vector<std::uint64_t> presentTicks;
        std::vector<std::uint64_t> encodeStartedUs;
        received.reserve(cadence::kSampleCapacity);
        sourceAges.reserve(cadence::kSampleCapacity);
        presentTicks.reserve(cadence::kSampleCapacity);
        encodeStartedUs.reserve(cadence::kSampleCapacity);
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
            throw std::runtime_error("Cannot measure DXGI present age");
        }
        unsigned sourceWidth = 0, sourceHeight = 0, targetRate = 0, hardwareFrames = 0, gpuFrames = 0, calls = 0, noUpdate = 0, cursorOnly = 0;
        std::uint64_t totalEncodeMicroseconds = 0, totalEncodedBytes = 0;
        ultraterimal::agent::DesktopVideoDiagnostics captured;
        StageSamples stages;
        std::exception_ptr failure;
        asio::co_spawn(attachment.executor(), measure(video, duration, sharp, regions, optimization, regionFrames, received, sourceAges, presentTicks, encodeStartedUs, frequency.QuadPart, sourceWidth, sourceHeight, targetRate, hardwareFrames, gpuFrames, totalEncodeMicroseconds, totalEncodedBytes, calls, noUpdate, cursorOnly, captured, stages, paintCounter), [&](std::exception_ptr error) {
            failure = error;
            video.stop();
            attachment.request_stop();
        });
        const auto cpuStarted = cpuTicks();
        io.run();
        io.restart();
        auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
        io.run();
        retired.get();
        painter.request_stop();
        painter.join();
        const auto cpuMilliseconds = (cpuTicks() - cpuStarted) / 10000.0;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            throw std::runtime_error("Cannot measure benchmark process memory");
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (painterFailure) {
            std::rethrow_exception(painterFailure);
        }
        const bool completeTimestamps = presentTicks.size() == received.size() && encodeStartedUs.size() == received.size();
        std::uint64_t nonadvancingSourceTimestamps = 0, nonadvancingEncodeTimestamps = 0;
        std::vector<double> gaps;
        std::vector<double> sourceGaps;
        std::vector<double> encodeStartGaps;
        unsigned longOutputGaps = 0, longOutputGapsGe66ms = 0, longOutputGapsGe100ms = 0, longSourceGaps = 0, longOutputWithShortSource = 0, longOutputWithShortEncodeStart = 0;
        for (std::size_t index = 1; index < received.size(); ++index) {
            const auto gap = std::chrono::duration<double, std::milli>(received[index] - received[index - 1]).count();
            gaps.push_back(gap);
            longOutputGaps += gap >= 45;
            longOutputGapsGe66ms += gap >= 66;
            longOutputGapsGe100ms += gap >= 100;
            if (completeTimestamps) {
                nonadvancingSourceTimestamps += presentTicks[index] <= presentTicks[index - 1];
                nonadvancingEncodeTimestamps += encodeStartedUs[index] <= encodeStartedUs[index - 1];
                if (presentTicks[index] >= presentTicks[index - 1]) {
                    const auto sourceGap = 1000.0 * (presentTicks[index] - presentTicks[index - 1]) / frequency.QuadPart;
                    sourceGaps.push_back(sourceGap);
                    longSourceGaps += sourceGap >= 45;
                    longOutputWithShortSource += gap >= 45 && sourceGap < 35;
                }
                if (encodeStartedUs[index] >= encodeStartedUs[index - 1]) {
                    const auto encodeStartGap = (encodeStartedUs[index] - encodeStartedUs[index - 1]) / 1000.0;
                    encodeStartGaps.push_back(encodeStartGap);
                    longOutputWithShortEncodeStart += gap >= 45 && encodeStartGap < 35;
                }
            }
        }
        const auto elapsed = received.size() >= 2 ? std::optional(std::chrono::duration<double>(received.back() - received.front()).count()) : std::nullopt;
        std::sort(gaps.begin(), gaps.end());
        std::sort(sourceGaps.begin(), sourceGaps.end());
        std::sort(encodeStartGaps.begin(), encodeStartGaps.end());
        std::sort(sourceAges.begin(), sourceAges.end());
        const auto action = scrolling ? "scroll" : motion ? "motion"
                                               : detail   ? "detail"
                                                          : "paint";
        const auto percentile = [&](const std::vector<double>& values, unsigned percent) -> std::optional<double> {
            if (values.empty() || stages.cadenceDrop) {
                return std::nullopt;
            }
            return values[(values.size() * percent + 99) / 100 - 1];
        };
        const auto emit = [&](std::string_view name, std::optional<double> value) {
            std::cout << ",\"" << name << "\":";
            cadence::number(std::cout, value);
        };
        const auto emitInteger = [&]<std::unsigned_integral Integer>(std::string_view name, std::optional<Integer> value) {
            std::cout << ",\"" << name << "\":";
            cadence::integer(std::cout, value);
        };
        std::cout << std::setprecision(17) << "{\"workload\":\"real_dxgi_paint_to_video_runtime\",\"ack_model\":\"local_immediate_software_ack\",\"optimization_contract\":" << (optimization ? 2 : 0) << ",\"paint_hz\":" << paintHz << ",\"quality\":\"" << (sharp ? "sharp" : "smooth")
                  << "\",\"surface_action\":\"" << action << "\",\"paint_size\":" << paintSize << ",\"source_width\":" << sourceWidth << ",\"source_height\":" << sourceHeight
                  << ",\"isolated_desktop\":" << (isolatedDesktop ? "true" : "false") << ",\"duration_seconds\":" << duration.count() << ",\"paint_calls\":" << paints
                  << ",\"target_fps\":" << targetRate << ",\"encoded_frames\":" << stages.videoUpdates;
        emit("measured_seconds", elapsed);
        emit("mean_encode_us", stages.videoUpdates ? std::optional(static_cast<double>(totalEncodeMicroseconds) / stages.videoUpdates) : std::nullopt);
        emit("mean_encoded_bytes", stages.videoUpdates ? std::optional(static_cast<double>(totalEncodedBytes) / stages.videoUpdates) : std::nullopt);
        emit("encoded_bps", elapsed && *elapsed > 0 && !stages.cadenceDrop ? std::optional(totalEncodedBytes * 8 / *elapsed) : std::nullopt);
        std::cout << ",\"region_frames\":" << regionFrames << ",\"hardware_frames\":" << hardwareFrames << ",\"gpu_frames\":" << gpuFrames
                  << ",\"video_next_calls\":" << calls << ",\"no_update_calls\":" << noUpdate << ",\"cursor_only_calls\":" << cursorOnly
                  << ",\"dxgi_calls\":" << captured.captureCalls << ",\"pixel_updates\":" << captured.pixelUpdates << ",\"dxgi_timeouts\":" << captured.captureTimeouts
                  << ",\"pending_before_deadline_calls\":" << captured.pendingBeforeDeadlineCalls << ",\"backpressured_calls\":" << captured.backpressuredCalls
                  << ",\"worker_encoded_frames\":" << captured.encodedFrames
                  << ",\"unknown_damage_updates\":" << captured.unknownDamageUpdates << ",\"region_candidates\":" << captured.regionCandidates << ",\"region_budget_failures\":" << captured.regionBudgetFailures
                  << ",\"region_time_budget_failures\":" << captured.regionTimeBudgetFailures << ",\"filtered_pixels\":" << captured.filteredPixels
                  << ",\"unchanged_pixel_updates\":" << captured.unchangedPixelUpdates << ",\"sparse_readback_pixels\":" << captured.sparseReadbackPixels
                  << ",\"pixel_capture_gaps\":" << captured.pixelCaptureGaps << ",\"pixel_capture_gaps_ge_45ms\":" << captured.pixelCaptureGapsGe45ms
                  << ",\"cpu_ms_including_warmup\":" << cpuMilliseconds << ",\"peak_working_set_kib\":" << memory.PeakWorkingSetSize / 1024
                  << ",\"private_bytes_kib\":" << memory.PrivateUsage / 1024;
        emit("effective_fps", elapsed && *elapsed > 0 && !stages.cadenceDrop ? std::optional((received.size() - 1) / *elapsed) : std::nullopt);
        emit("gap_p05_ms", percentile(gaps, 5));
        emit("gap_p50_ms", gaps.empty() || stages.cadenceDrop ? std::nullopt : std::optional(gaps[gaps.size() / 2]));
        emit("gap_p95_ms", percentile(gaps, 95));
        emit("gap_p99_ms", percentile(gaps, 99));
        emit("source_gap_p95_ms", percentile(sourceGaps, 95));
        emit("source_gap_p99_ms", percentile(sourceGaps, 99));
        emit("encode_start_gap_p95_ms", percentile(encodeStartGaps, 95));
        emit("encode_start_gap_p99_ms", percentile(encodeStartGaps, 99));
        const bool outputCountersComplete = !stages.cadenceDrop;
        const bool sourceCountersComplete = outputCountersComplete && completeTimestamps;
        emitInteger("output_gaps_ge_45ms", outputCountersComplete ? std::optional(longOutputGaps) : std::nullopt);
        emitInteger("output_gaps_ge_66ms", outputCountersComplete ? std::optional(longOutputGapsGe66ms) : std::nullopt);
        emitInteger("output_gaps_ge_100ms", outputCountersComplete ? std::optional(longOutputGapsGe100ms) : std::nullopt);
        emitInteger("source_gaps_ge_45ms", sourceCountersComplete ? std::optional(longSourceGaps) : std::nullopt);
        emitInteger("output_gaps_ge_45ms_with_source_lt_35ms", sourceCountersComplete ? std::optional(longOutputWithShortSource) : std::nullopt);
        emitInteger("output_gaps_ge_45ms_with_encode_start_lt_35ms", sourceCountersComplete ? std::optional(longOutputWithShortEncodeStart) : std::nullopt);
        emit("source_age_p50_ms", sourceAges.empty() || stages.cadenceDrop ? std::nullopt : std::optional(sourceAges[sourceAges.size() / 2]));
        emit("source_age_p95_ms", percentile(sourceAges, 95));
        emit("source_age_p99_ms", percentile(sourceAges, 99));
        std::cout << ",\"refinement_batches\":" << stages.refinementBatches << ",\"refinement_tiles\":" << stages.refinementTiles << ",\"refinement_bytes\":" << stages.refinementBytes
                  << ",\"dynamic_refinement_batches\":" << captured.dynamicRefinementBatches << ",\"roi_submissions\":" << captured.roiSubmissions << ",\"roi_pending\":" << captured.roiPending
                  << ",\"roi_busy\":" << captured.roiBusy << ",\"roi_retired\":" << captured.roiRetired << ",\"roi_failures\":" << captured.roiFailures << ",\"optional_cost_denials\":" << captured.optionalCostDenials;
        for (const auto& field : kOptionalCounters) {
            std::cout << ",\"optional_" << field.first << "\":" << captured.optional.*(field.second);
        }
        emitInteger("optional_last_predicted_message_bytes", captured.optional.predictedMessageBytes);
        emitInteger("optional_last_predicted_prepare_us", captured.optional.predictedPrepareMicroseconds);
        emitInteger("optional_last_actual_png_bytes", captured.optional.lastPngBytes);
        stages.primary.write(std::cout);
        stages.staticDetail.write(std::cout, "static_detail_");
        std::cout << std::boolalpha << ",\"benchmark_schema_version\":2,\"test_only\":true,\"agent_version\":";
        jsonString(std::cout, ultraterimal::agent::kAgentVersion);
        std::cout << ",\"compiled_at\":";
        jsonString(std::cout, __DATE__ " " __TIME__);
        std::cout << ",\"compiler_msvc_full_version\":" << _MSC_FULL_VER
                  << ",\"ack_is_network_or_physical\":false,\"synthetic_feedback_decode_us\":1000"
                  << ",\"stage_sample_policy\":\"per_update_primary_then_missing_dynamic_fields;static_separate\""
                  << ",\"stage_percentile_policy\":\"exact_nearest_rank;null_on_overflow\""
                  << ",\"sample_capacity_per_stream\":" << cadence::kSampleCapacity
                  << ",\"cadence_sample_drop\":" << stages.cadenceDrop
                  << ",\"cadence_timestamps_complete\":" << completeTimestamps;
        emitInteger("nonadvancing_source_timestamps", sourceCountersComplete ? std::optional(nonadvancingSourceTimestamps) : std::nullopt);
        emitInteger("nonadvancing_encode_timestamps", sourceCountersComplete ? std::optional(nonadvancingEncodeTimestamps) : std::nullopt);
        std::cout << ",\"static\":" << stationary << ",\"full_desktop_requested\":" << fullDesktop
                  << ",\"measurement_window_seconds\":" << stages.measurementWindowSeconds
                  << ",\"output_updates_per_measurement_second\":" << stages.videoUpdates / stages.measurementWindowSeconds
                  << ",\"no_primary_output_tail_ms\":" << stages.noPrimaryOutputTailMs
                  << ",\"diagnostic_snapshot_includes_final_next\":true"
                  << ",\"requested_capabilities\":{\"codecs\":[\"AVC\"],\"region_updates\":" << regions << ",\"tile_cache\":" << regions << ",\"desktop_optimization_contract\":" << (optimization ? 2 : 0) << '}';
        const auto rectangle = [&](const RECT& rect) {
            std::cout << "{\"left\":" << rect.left << ",\"top\":" << rect.top << ",\"right\":" << rect.right << ",\"bottom\":" << rect.bottom << '}';
        };
        std::cout << ",\"surface_actual_rectangle\":";
        rectangle(surfaceEvidence.rectangle);
        std::cout << ",\"surface_display_rectangle\":";
        rectangle(surfaceEvidence.desktop);
        std::cout << ",\"surface_visible_at_last_inspection\":" << surfaceEvidence.visible
                  << ",\"surface_center_on_top_at_last_inspection\":" << surfaceEvidence.centerOnTop
                  << ",\"surface_static_wm_paint_count\":";
        if (stationary) {
            std::cout << surfaceEvidence.wmPaints;
        } else {
            std::cout << "null"; // Existing fixture does not expose a WM_PAINT counter.
        }
        std::cout << ",\"surface_covers_last_config_desktop\":";
        if (!stages.configurations.empty() && !stages.configMetaDrop) {
            const auto& config = stages.configurations.back();
            const auto& rect = surfaceEvidence.rectangle;
            const bool covers = rect.left <= config.left && rect.top <= config.top &&
                                static_cast<std::int64_t>(rect.right) >= static_cast<std::int64_t>(config.left) + config.config.width() &&
                                static_cast<std::int64_t>(rect.bottom) >= static_cast<std::int64_t>(config.top) + config.config.height();
            std::cout << covers;
        } else {
            std::cout << "null";
        }
        constexpr std::array bodyNames{"video_frame", "video_refinement", "desktop_region_update", "video_config", "cursor_update", "local_quality_feedback", "local_refinement_ack"};
        std::uint64_t outputPayload = 0, outputBody = 0;
        std::cout << ",\"message_bytes_scope\":\"measurement_window;protobuf_body_not_envelope_or_network\",\"message_bytes\":{";
        const auto writeBody = [&](const BodyBytes& body) {
            std::cout << "{\"count\":" << body.count << ",\"payload\":" << body.payload << ",\"serializedBody\":" << body.serializedBody << '}';
        };
        for (std::size_t index = 0; index < bodyNames.size(); ++index) {
            if (index) {
                std::cout << ',';
            }
            jsonString(std::cout, bodyNames[index]);
            std::cout << ':';
            writeBody(stages.bodies[index]);
            if (index < 5) {
                outputPayload += stages.bodies[index].payload;
                outputBody += stages.bodies[index].serializedBody;
            }
        }
        std::cout << "},\"initial_control_body_bytes\":{\"local_quality_feedback\":";
        writeBody(stages.initialFeedback);
        std::cout << ",\"local_capabilities\":";
        writeBody(stages.initialCapabilities);
        std::cout << "},\"total_encoded_bytes\":" << totalEncodedBytes
                  << ",\"total_payload_bytes\":" << outputPayload << ",\"total_serialized_body_bytes\":" << outputBody
                  << ",\"all_output_body_bps_over_measurement_window\":" << outputBody * 8 / stages.measurementWindowSeconds
                  << ",\"observed_static_refinement_batches\":" << stages.staticBatches
                  << ",\"observed_dynamic_refinement_batches\":" << stages.dynamicBatches
                  << ",\"observed_legacy_refinement_batches\":" << stages.legacyBatches
                  << ",\"refinement_evidence_scope\":\"baseline_warmup_measurement_and_final_next;measured_flag_per_batch\""
                  << ",\"all_observed_static_refinement_batches\":" << stages.observedStaticBatches
                  << ",\"all_observed_dynamic_refinement_batches\":" << stages.observedDynamicBatches
                  << ",\"refinement_meta_drop\":" << stages.refinementMetaDrop
                  << ",\"static_coverage_overflow\":" << stages.staticCoverage.overflow()
                  << ",\"dynamic_coverage_overflow\":" << stages.dynamicCoverage.overflow();
        std::cout << ",\"static_idle\":";
        if (stationary) {
            std::cout << "{\"window_seconds\":" << stages.measurementWindowSeconds << ",\"video_or_region_updates\":" << stages.videoUpdates
                      << ",\"primaryless_next_calls\":" << calls - stages.videoUpdates << ",\"no_update_calls\":" << noUpdate
                      << ",\"pixel_capture_updates\":" << captured.pixelUpdates << ",\"refinement_batches\":" << stages.refinementBatches
                      << ",\"serialized_output_body_bytes\":" << outputBody
                      << ",\"no_primary_output_tail_in_window_ms\":" << std::min(stages.noPrimaryOutputTailMs, stages.measurementWindowSeconds * 1000) << '}';
        } else {
            std::cout << "null";
        }
        const auto staticMaximum = stages.staticCoverage.maximumPerBase();
        emitInteger("static_refinement_unique_grid", stages.staticCoverage.unique());
        emitInteger("dynamic_refinement_unique_grid", stages.dynamicCoverage.unique());
        emitInteger("static_refinement_max_grid_per_generation_frame", staticMaximum);
        std::cout << ",\"static_refinement_coverage_gt_32\":";
        if (staticMaximum) {
            std::cout << (*staticMaximum > 32);
        } else {
            std::cout << "null";
        }
        std::cout << ",\"refinement_meta\":[";
        for (std::size_t index = 0; index < stages.refinements.size(); ++index) {
            const auto& meta = stages.refinements[index];
            if (index) {
                std::cout << ',';
            }
            std::cout << "{\"generation\":" << meta.generation << ",\"frame\":" << meta.frame << ",\"batch\":" << meta.batch
                      << ",\"kind\":" << meta.kind << ",\"tiles\":" << meta.tiles << ",\"measured\":" << meta.measured
                      << ",\"payload\":" << meta.payload << ",\"serializedBody\":" << meta.serializedBody << ",\"unique_grid\":";
            cadence::integer(std::cout, meta.uniqueGrid);
            std::cout << ",\"span\":";
            if (meta.tiles) {
                std::cout << "{\"left\":" << meta.left << ",\"top\":" << meta.top << ",\"right\":" << meta.right << ",\"bottom\":" << meta.bottom << '}';
            } else {
                std::cout << "null";
            }
            std::cout << '}';
        }
        std::cout << "],\"config_evidence_scope\":\"baseline_warmup_measurement_and_final_next\",\"config_meta_drop\":" << stages.configMetaDrop << ",\"config_meta\":[";
        for (std::size_t index = 0; index < stages.configurations.size(); ++index) {
            const auto& meta = stages.configurations[index];
            if (index) {
                std::cout << ',';
            }
            std::cout << "{\"generation\":" << meta.generation << ",\"frame\":" << meta.frame << ",\"desktop_left\":" << meta.left << ",\"desktop_top\":" << meta.top
                      << ",\"codec\":";
            jsonString(std::cout, meta.config.codec());
            std::cout << ",\"width\":" << meta.config.width() << ",\"height\":" << meta.config.height()
                      << ",\"desktop_optimization\":" << meta.config.desktop_optimization() << ",\"desktop_optimization_contract\":" << meta.config.desktop_optimization_contract()
                      << ",\"description_bytes\":" << meta.config.description().size() << ",\"display_mode_count\":" << meta.config.display_modes_size()
                      << ",\"serializedBody\":" << meta.config.ByteSizeLong() << '}';
        }
        std::cout << ']';
        writeDiagnosticEvidence(std::cout, stages);
        std::cout << ",\"paint_calls_scope\":\"manual_painter_calls_including_setup_warmup;not_wm_paint_or_rgb_change\"";
        stages.paintMeasurement.write(std::cout);
        std::cout << "}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
