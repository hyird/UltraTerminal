#include "desktop.runtime.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace ultraterimal::server {
namespace {
std::string_view closeReasonName(DesktopCloseReason reason) {
    switch (reason) {
    case DesktopCloseReason::kServerStopped:
        return "server_stopped";
    case DesktopCloseReason::kUserClosed:
        return "user_closed";
    case DesktopCloseReason::kPreempted:
        return "preempted";
    case DesktopCloseReason::kDeviceDeleted:
        return "device_deleted";
    case DesktopCloseReason::kPeerClosed:
        return "peer_closed";
    case DesktopCloseReason::kPeerNonBinary:
        return "peer_non_binary";
    case DesktopCloseReason::kReadFailed:
        return "read_failed";
    case DesktopCloseReason::kWriteFailed:
        return "write_failed";
    case DesktopCloseReason::kSetupTimeout:
        return "setup_timeout";
    case DesktopCloseReason::kPermissionRevoked:
        return "permission_revoked";
    case DesktopCloseReason::kAuthorizationCheckFailed:
        return "authorization_check_failed";
    case DesktopCloseReason::kAttachRejected:
        return "attach_rejected";
    case DesktopCloseReason::kMalformedMessage:
        return "malformed_message";
    case DesktopCloseReason::kInvalidBinding:
        return "invalid_binding";
    case DesktopCloseReason::kInvalidDirection:
        return "invalid_direction";
    case DesktopCloseReason::kReadOnlyInput:
        return "read_only_input";
    case DesktopCloseReason::kMessageReplay:
        return "message_replay";
    case DesktopCloseReason::kInvalidFrameState:
        return "invalid_frame_state";
    case DesktopCloseReason::kSessionBudgetExceeded:
        return "session_budget_exceeded";
    case DesktopCloseReason::kGlobalBudgetExceeded:
        return "global_budget_exceeded";
    case DesktopCloseReason::kQueueClosed:
        return "queue_closed";
    case DesktopCloseReason::kReadyQueueClosed:
        return "ready_queue_closed";
    }
    return "unknown";
}
std::string_view closeOriginName(DesktopCloseOrigin origin) {
    switch (origin) {
    case DesktopCloseOrigin::kServer:
        return "server";
    case DesktopCloseOrigin::kBrowserInput:
        return "browser_input";
    case DesktopCloseOrigin::kAgentInput:
        return "agent_input";
    case DesktopCloseOrigin::kBrowserVideo:
        return "browser_video";
    case DesktopCloseOrigin::kAgentVideo:
        return "agent_video";
    }
    return "unknown";
}
DesktopCloseOrigin originForSlot(std::size_t slot) {
    constexpr std::array origins{DesktopCloseOrigin::kBrowserInput, DesktopCloseOrigin::kAgentInput, DesktopCloseOrigin::kBrowserVideo, DesktopCloseOrigin::kAgentVideo};
    return origins[slot];
}
DesktopCloseReason validationReason(protocol::DesktopMessageError error) {
    switch (error) {
    case protocol::DesktopMessageError::kMalformed:
        return DesktopCloseReason::kMalformedMessage;
    case protocol::DesktopMessageError::kBinding:
        return DesktopCloseReason::kInvalidBinding;
    case protocol::DesktopMessageError::kDirection:
        return DesktopCloseReason::kInvalidDirection;
    case protocol::DesktopMessageError::kReadOnly:
        return DesktopCloseReason::kReadOnlyInput;
    case protocol::DesktopMessageError::kReplay:
        return DesktopCloseReason::kMessageReplay;
    case protocol::DesktopMessageError::kFrameState:
        return DesktopCloseReason::kInvalidFrameState;
    }
    return DesktopCloseReason::kMalformedMessage;
}
} // namespace
DesktopDelivery::~DesktopDelivery() {
    if (local_) {
        local_->used -= bytes_.size();
        global_->used -= bytes_.size();
    }
}
DesktopEndpoint::DesktopEndpoint(ruvia::worker_handle worker, protocol::DesktopChannelBinding binding)
    : validator_(binding), queue_(ruvia::make_channel<std::unique_ptr<DesktopDelivery>>(worker, {.capacity_ = binding.purpose == wire::DESKTOP_VIDEO ? 2u : 8u})),
      space_(ruvia::make_channel<bool>(std::move(worker), {.capacity_ = 1})) {}
ruvia::task<std::unique_ptr<DesktopDelivery>> DesktopEndpoint::receive(ruvia::stop_token stop) {
    auto result = co_await queue_.second.receive(std::move(stop));
    if (!result.has_value()) {
        co_return nullptr;
    }
    auto delivery = std::move(result).take_value();
    delivery->received_ = std::chrono::steady_clock::now();
    if (waiting_) {
        (void)space_.first.send(true);
    }
    if (closed_) {
        delivery.reset();
        // close() wakes the single writer; release its remaining queue before it exits.
        while ((co_await queue_.second.receive()).has_value()) {
        }
        co_return nullptr;
    }
    co_return delivery;
}
void DesktopEndpoint::close() {
    closed_ = true;
    queue_.first.close();
    space_.first.close();
}
DesktopSession::DesktopSession(ruvia::worker_handle worker, std::shared_ptr<DesktopBudget> global)
    : worker_(std::move(worker)), global_(std::move(global)) {
    if (!global_) {
        throw std::invalid_argument("Desktop session requires a Worker budget");
    }
}
DesktopSession::~DesktopSession() { close(); }
std::shared_ptr<DesktopEndpoint> DesktopSession::attach(const ChannelAuthorization& authorization, protocol::ChannelSource source) {
    if (closed_ || authorization.kind != wire::DESKTOP || authorization.sessionId.empty() || authorization.channelId.empty() ||
        (authorization.purpose != wire::DESKTOP_INPUT && authorization.purpose != wire::DESKTOP_VIDEO) ||
        (source != protocol::ChannelSource::kBrowser && source != protocol::ChannelSource::kAgent)) {
        return {};
    }
    if (bound_ && (identity_.sessionId != authorization.sessionId || identity_.userId != authorization.userId ||
                   identity_.deviceId != authorization.deviceId || identity_.writeControl != authorization.writeControl)) {
        return {};
    }
    const std::size_t channel = authorization.purpose == wire::DESKTOP_INPUT ? 0 : 1;
    const std::size_t slot = channel * 2 + (source == protocol::ChannelSource::kAgent ? 1 : 0);
    if (endpoints_[slot] || (!channels_[channel].empty() && channels_[channel] != authorization.channelId) ||
        channels_[1 - channel] == authorization.channelId) {
        return {};
    }
    auto endpoint = std::shared_ptr<DesktopEndpoint>(new DesktopEndpoint(worker_, {authorization.sessionId, authorization.channelId, authorization.purpose, source, authorization.writeControl}));
    if (!bound_) {
        identity_ = authorization;
        bound_ = true;
    }
    channels_[channel] = authorization.channelId;
    endpoints_[slot] = endpoint;
    if (std::ranges::all_of(endpoints_, [](const auto& item) { return bool(item); })) {
        for (auto& item : endpoints_) {
            if (!item->queue_.first.send(std::unique_ptr<DesktopDelivery>(new DesktopDelivery())).accepted()) {
                close(DesktopCloseReason::kReadyQueueClosed);
                return {};
            }
        }
        ready_ = true;
        spdlog::info("desktop_session_ready session_id={} device_id={}", identity_.sessionId, identity_.deviceId);
    }
    return endpoint;
}
ruvia::task<bool> DesktopSession::forward(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::stop_token stop) {
    if (!worker_.is_current()) {
        throw std::logic_error("Desktop session accessed outside its Worker");
    }
    const auto found = std::ranges::find(endpoints_, sender);
    if (!ready() || !sender || found == endpoints_.end()) {
        co_return false;
    }
    const auto admissionStarted = std::chrono::steady_clock::now();
    const auto slot = static_cast<std::size_t>(found - endpoints_.begin());
    const auto origin = originForSlot(slot);
    if (!optimizationActive_ &&
        ((message.has_video_frame() && message.video_frame().has_stage_timing()) ||
         (message.has_desktop_region_update() && message.desktop_region_update().has_stage_timing()) ||
         (message.has_video_refinement() && message.video_refinement().has_stage_timing()))) {
        close(DesktopCloseReason::kInvalidFrameState, origin);
        co_return false;
    }
    if (const auto valid = sender->validator_.accept(message); !valid) {
        close(validationReason(valid.error()), origin);
        co_return false;
    }
    if (message.has_video_config()) {
        optimizationBarrier(message);
        diagnostics_.pipeline.generation(message.generation());
    }
    if (message.has_desktop_video_capabilities()) {
        browserOptimizationRequest_ = message.desktop_video_capabilities().desktop_optimization_contract();
        // Only the exact request permits the NEXT configuration to activate v2.
        endpoints_[3]->validator_.setOptimizationContract(browserOptimizationRequest_ == 2 ? 2 : 0);
        endpoints_[3]->validator_.setContinuousFrames(message.desktop_video_capabilities().continuous_frames());
    }
    if (message.has_desktop_pipeline_report()) {
        if (!optimizationActive_ && browserOptimizationRequest_ != 2) {
            // Unnegotiated diagnostics never activate v2 or enter a peer queue.
            diagnostics_.pipeline.dropped += message.desktop_pipeline_report().samples_size();
            co_return true;
        }
        // Authenticated diagnostics can overtake config on the other stream.
        // Keys become observable ONLY after a matching v2 config and frame;
        // capability-missing/high-version frames never match buffered samples.
        diagnostics_.pipeline.report(message.desktop_pipeline_report());
        for (const auto& sample : message.desktop_pipeline_report().samples()) {
            if (sample.has_flow_sample() && sample.source() == wire::DesktopPipelineSample::DESKTOP_WORKER && !sample.batch_id() &&
                sample.generation() == diagnostics_.videoGeneration && diagnostics_.pipeline.known({sample.generation(), sample.frame_id(), 0})) {
                cadence_.observeFlow(sample.flow_sample(), sample.generation(), sample.frame_id());
            }
        }
        logCadence(std::chrono::steady_clock::now());
        co_return true; // Never enters the browser queue or creates more diagnostics.
    }
    if (outstandingRefinement_ && std::chrono::steady_clock::now() - outstandingRefinement_->sent >= std::chrono::seconds(2)) {
        pausedRefinementGeneration_ = outstandingRefinement_->key.generation;
        // Keep the exact unacknowledged tuple, including across generation
        // barriers. Timeout cannot prove the Browser's PNG lease has ended.
    }
    if (message.has_desktop_refinement_ack()) {
        const auto result = refinementAck(message);
        if (result == RefinementAckResult::kInvalid) {
            close(DesktopCloseReason::kInvalidFrameState, origin);
            co_return false;
        }
        if (result == RefinementAckResult::kIgnore) {
            co_return true;
        }
    }
    if (message.has_video_refinement() && (outstandingRefinement_ || (optimizationActive_ && pausedRefinementGeneration_ == message.generation()))) {
        // A new video frame may retire a batch, but cannot manufacture credit.
        close(DesktopCloseReason::kInvalidFrameState, origin);
        co_return false;
    }
    // Browser diagnostics terminate here; they never consume worker IPC or
    // influence Agent QoS. Strip only present diagnostics, preserving QoS fields.
    std::optional<wire::Envelope> forwarded;
    if (message.has_quality_feedback() && message.quality_feedback().has_browser_timing()) {
        const auto& feedback = message.quality_feedback();
        if (optimizationActive_ && !feedback.background() && diagnostics_.pipeline.known({feedback.displayed_generation(), feedback.displayed_frame(), 0})) {
            diagnostics_.pipeline.observe(feedback.browser_timing());
        }
        forwarded = message;
        forwarded->mutable_quality_feedback()->clear_browser_timing();
    }
    if (message.has_quality_feedback() && message.quality_feedback().has_visible_frame_timing()) {
        const auto& feedback = message.quality_feedback();
        const auto now = std::chrono::steady_clock::now();
        if (!feedback.background() && feedback.displayed_generation() == diagnostics_.videoGeneration &&
            feedback.displayed_frame() && feedback.displayed_frame() <= diagnostics_.latestVideoFrame &&
            (diagnostics_.renderReportedAt == std::chrono::steady_clock::time_point{} || now - diagnostics_.renderReportedAt >= std::chrono::seconds(4))) {
            const auto& report = feedback.visible_frame_timing();
            diagnostics_.visibleFrames.observe(report.sample_count(), report.p95_microseconds(), report.p99_microseconds(), report.long_gap_count(), report.observed_fps_milli());
            diagnostics_.renderReportedAt = now;
        }
        if (!forwarded) {
            forwarded = message;
        }
        forwarded->mutable_quality_feedback()->clear_visible_frame_timing();
    }
    auto bytes = protocol::encodeMessage(forwarded ? *forwarded : message);
    if (!bytes) {
        close(DesktopCloseReason::kMalformedMessage, origin);
        co_return false;
    }
    if (bytes->size() > local_->limit - local_->used) {
        close(DesktopCloseReason::kSessionBudgetExceeded, origin);
        co_return false;
    }
    if (bytes->size() > global_->limit - global_->used) {
        close(DesktopCloseReason::kGlobalBudgetExceeded, origin);
        co_return false;
    }
    auto delivery = std::unique_ptr<DesktopDelivery>(new DesktopDelivery());
    delivery->bytes_ = std::move(*bytes);
    delivery->browserVideo_ = slot == 3;
    delivery->primaryVideo_ = message.has_video_frame() || message.has_desktop_region_update() || message.has_full_frame() || message.has_diff_batch();
    delivery->local_ = local_;
    delivery->global_ = global_;
    local_->used += delivery->bytes_.size();
    global_->used += delivery->bytes_.size();
    diagnostics_.peakPendingBytes = std::max(diagnostics_.peakPendingBytes, local_->used);
    const auto target = endpoints_[slot ^ 1];
    while (ready() && !stop.stop_requested()) {
        const auto size = delivery->bytes_.size();
        delivery->enqueued_ = std::chrono::steady_clock::now();
        auto sent = target->queue_.first.send(std::move(delivery));
        if (sent.accepted()) {
            const auto admittedAt = std::chrono::steady_clock::now();
            if (slot == 3) {
                diagnostics_.pipeline.observe(DesktopStage::kServerVideoAdmission, static_cast<std::uint32_t>(std::min<std::int64_t>(60000000, std::chrono::duration_cast<std::chrono::microseconds>(admittedAt - admissionStarted).count())));
            }
            ++diagnostics_.forwardedMessages;
            diagnostics_.forwardedBytes += size;
            diagnostics_.videoFrames += message.has_video_frame() || message.has_full_frame() || message.has_diff_batch() || message.has_desktop_region_update();
            diagnostics_.inputEvents += message.has_input_event();
            if (message.has_video_config()) {
                cadence_ = {};
                diagnostics_.videoGeneration = message.generation();
                diagnostics_.latestVideoFrame = 0;
                diagnostics_.renderReportedAt = {};
                diagnostics_.visibleFrames.clear();
                diagnostics_.encoding.clear();
                diagnostics_.videoWidth = message.video_config().width();
                diagnostics_.videoHeight = message.video_config().height();
                diagnostics_.latestKeyFrameEncodeMicroseconds = 0;
                diagnostics_.latestTargetFramesPerSecond = 0;
                diagnostics_.latestTargetBitrate = 0;
                diagnostics_.latestHardwareEncoding = false;
                diagnostics_.latestGpuEncoding = false;
            }
            if (message.has_full_frame() && message.generation() != diagnostics_.videoGeneration) {
                // Lossless fallback retires submission eligibility, not the
                // unacknowledged v2 lease or its cleanup ACK route.
                optimizationActive_ = false;
                endpoints_[0]->validator_.setOptimizationContract(0);
                endpoints_[1]->validator_.setOptimizationContract(0);
                diagnostics_.pipeline.generation(message.generation(), admittedAt);
                diagnostics_.videoGeneration = message.generation();
            }
            if (message.has_video_frame() || message.has_desktop_region_update() || message.has_full_frame() || message.has_diff_batch()) {
                if (optimizationActive_) {
                    diagnostics_.pipeline.frame({message.generation(), message.frame_id(), 0}, admittedAt);
                }
            }
            if (message.has_video_frame()) {
                const auto& video = message.video_frame();
                cadence_.frame(video.stage_timing(), admittedAt);
                cadence_.keyFrames += video.key_frame();
                cadence_.keyBytes += video.key_frame() ? video.data().size() : 0;
                cadence_.videoBytes += video.data().size();
                cadence_.repeatedFrames += video.repeated();
                // v2 writes the final cumulative worker stages after dynamic
                // detail preparation. A present encode stage (including zero)
                // identifies that complete sample, unlike an absent/empty body.
                cumulativeVideoStages_ = optimizationActive_ && message.video_frame().has_stage_timing() && message.video_frame().stage_timing().has_video_encode_microseconds()
                                             ? DesktopFrameKey{message.generation(), message.frame_id(), 0}
                                             : DesktopFrameKey{};
                if (optimizationActive_ && message.video_frame().has_stage_timing()) {
                    diagnostics_.pipeline.observe(message.video_frame().stage_timing());
                }
                diagnostics_.latestVideoFrame = message.frame_id();
                const auto& frame = message.video_frame();
                diagnostics_.encoding.observe(frame.encode_microseconds());
                diagnostics_.latestHardwareEncoding = frame.hardware_encoding();
                diagnostics_.latestGpuEncoding = frame.gpu_encoding();
                diagnostics_.latestTargetFramesPerSecond = frame.target_frames_per_second();
                diagnostics_.latestTargetBitrate = frame.target_bitrate();
                if (frame.key_frame()) {
                    diagnostics_.latestKeyFrameEncodeMicroseconds = frame.encode_microseconds();
                }
            }
            if (message.has_desktop_region_update()) {
                const auto& update = message.desktop_region_update();
                cadence_.frame(update.stage_timing(), admittedAt);
                if (optimizationActive_ && message.desktop_region_update().has_stage_timing()) {
                    diagnostics_.pipeline.observe(message.desktop_region_update().stage_timing());
                }
                diagnostics_.latestVideoFrame = message.frame_id();
                ++diagnostics_.regionMessages;
                diagnostics_.regionBytes += size;
                diagnostics_.latestTargetFramesPerSecond = message.desktop_region_update().target_frames_per_second();
                diagnostics_.latestTargetBitrate = message.desktop_region_update().target_bitrate();
                for (const auto& copy : message.desktop_region_update().copies()) {
                    diagnostics_.copiedPixels += static_cast<std::uint64_t>(copy.destination().width()) * copy.destination().height();
                }
                for (const auto& patch : message.desktop_region_update().patches()) {
                    diagnostics_.tileCacheHits += patch.cache_slot() && patch.png().empty();
                }
            }
            if (message.has_quality_feedback()) {
                ++diagnostics_.feedbackMessages;
                const auto& feedback = message.quality_feedback();
                diagnostics_.maximumDecodeMicroseconds = std::max(diagnostics_.maximumDecodeMicroseconds, feedback.decode_microseconds());
                diagnostics_.maximumBrowserQueueDepth = std::max(diagnostics_.maximumBrowserQueueDepth, feedback.queue_depth());
                diagnostics_.latestBrowserQueueDepth = feedback.queue_depth();
                diagnostics_.latestBackground = feedback.background();
                if (feedback.displayed_generation() == diagnostics_.videoGeneration && feedback.displayed_frame() <= diagnostics_.latestVideoFrame) {
                    cadence_.feedback(feedback);
                }
                if (feedback.background()) {
                    diagnostics_.renderReportedAt = {};
                    diagnostics_.visibleFrames.clear();
                }
            }
            if (message.has_video_refinement()) {
                if (optimizationActive_) {
                    const DesktopFrameKey key{message.generation(), message.frame_id(), message.video_refinement().batch_id()};
                    outstandingRefinement_ = RefinementRecord{key, admittedAt};
                    diagnostics_.pipeline.frame(key, admittedAt);
                    const bool duplicateWorkerStages = message.video_refinement().kind() == wire::DYNAMIC_DETAIL &&
                                                       cumulativeVideoStages_ == DesktopFrameKey{message.generation(), message.frame_id(), 0};
                    if (message.video_refinement().has_stage_timing() && !duplicateWorkerStages) {
                        diagnostics_.pipeline.observe(message.video_refinement().stage_timing());
                    }
                }
                ++diagnostics_.refinementMessages;
                diagnostics_.refinementBytes += size;
                for (const auto& patch : message.video_refinement().patches()) {
                    diagnostics_.tileCacheHits += patch.cache_slot() && patch.png().empty();
                }
            }
            cadence_.inputs += message.has_input_event();
            logCadence(admittedAt);
            co_return true;
        }
        if (sent.status() != ruvia::channel_send_status::full) {
            break;
        }
        // Keep ownership and accounting while the downstream writer is busy.
        // Waiting suspends this channel's reader, so no input or frame is lost.
        delivery = std::move(*sent.rejected());
        ++diagnostics_.backpressureEvents;
        const auto waitStarted = std::chrono::steady_clock::now();
        target->waiting_ = true;
        const auto available = co_await target->space_.second.receive(stop);
        target->waiting_ = false;
        diagnostics_.backpressureMilliseconds += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStarted).count();
        if (!available.has_value()) {
            break;
        }
    }
    close(DesktopCloseReason::kQueueClosed, origin);
    co_return false;
}
void DesktopSession::optimizationBarrier(const wire::Envelope& message) {
    // The validator retires the old canvas. Preserve the sent tuple and its
    // original deadline: a new config is NOT an auxiliary-lease completion.
    // The timeout marker remains generation-scoped; outstanding credit remains
    // session-scoped until an exact cleanup ACK, regardless of this barrier.
    cumulativeVideoStages_ = {};
    agentOptimizationCapability_ = message.video_config().desktop_optimization_contract();
    optimizationActive_ = browserOptimizationRequest_ == 2 && agentOptimizationCapability_ == 2;
    // Both native and WSS ingress use these same authenticated endpoints.
    endpoints_[0]->validator_.setOptimizationContract(optimizationActive_ ? 2 : 0);
    endpoints_[1]->validator_.setOptimizationContract(optimizationActive_ ? 2 : 0);
}
void DesktopSession::completeRefinement() {
    if (outstandingRefinement_) {
        evictedRefinement_ = retiredRefinements_[nextRetiredRefinement_];
        retiredRefinements_[nextRetiredRefinement_] = outstandingRefinement_->key;
        nextRetiredRefinement_ = (nextRetiredRefinement_ + 1) % retiredRefinements_.size();
        outstandingRefinement_.reset();
    }
}
DesktopSession::RefinementAckResult DesktopSession::refinementAck(const wire::Envelope& message) {
    const DesktopFrameKey key{message.generation(), message.frame_id(), message.desktop_refinement_ack().batch_id()};
    // Beyond bounded exact tombstones, a monotonically older ACK is harmless:
    // it is consumed, never forwarded and can never release any credit.
    if (evictedRefinement_.generation && (key.generation < evictedRefinement_.generation ||
                                          (key.generation == evictedRefinement_.generation && key.batch <= evictedRefinement_.batch))) {
        return RefinementAckResult::kIgnore;
    }
    if (outstandingRefinement_ && outstandingRefinement_->key == key) {
        // Cleanup releases the exact lease even after a config/frame change or
        // timeout. Forward once; never synthesize a video QualityFeedback ACK.
        diagnostics_.refinementApplied += message.desktop_refinement_ack().status() == wire::DesktopRefinementAck::APPLIED;
        diagnostics_.refinementSuperseded += message.desktop_refinement_ack().status() == wire::DesktopRefinementAck::SUPERSEDED;
        diagnostics_.refinementAcknowledgement.observePresent(static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - outstandingRefinement_->sent).count(), 0, 60000000)));
        completeRefinement();
        return RefinementAckResult::kForward;
    }
    if (std::ranges::find(retiredRefinements_, key) != retiredRefinements_.end()) {
        return RefinementAckResult::kIgnore;
    }
    return RefinementAckResult::kInvalid;
}
void DesktopSession::writeCompleted(const DesktopDelivery& delivery, std::chrono::steady_clock::duration elapsed) {
    if (!delivery.browserVideo_ || delivery.ready()) {
        return;
    }
    const auto micros = [](auto value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(value).count(), 0, 60000000));
    };
    diagnostics_.pipeline.observe(DesktopStage::kServerVideoQueueWait, micros(delivery.received_ - delivery.enqueued_));
    diagnostics_.pipeline.observe(DesktopStage::kServerVideoWsWrite, micros(elapsed));
    if (delivery.primaryVideo_) {
        cadence_.begin(std::chrono::steady_clock::now());
        ++cadence_.written;
        cadence_.writes.observePresent(micros(elapsed));
    }
}
void DesktopSession::logCadence(std::chrono::steady_clock::time_point now) {
    if (!cadence_.due(now)) {
        return;
    }
    const auto gap = cadence_.gaps.summary(), acquire = cadence_.acquire.summary(), map = cadence_.map.summary(), encode = cadence_.encode.summary();
    const auto dispatch = cadence_.dispatch.summary(), credit = cadence_.credit.summary(), write = cadence_.writes.summary();
    const auto inputApply = cadence_.inputApply.summary(), loopLate = cadence_.loopLate.summary();
    spdlog::info("desktop_fps_sample session_id={} generation={} window_ms={} received_fps_milli={} written_fps_milli={} displayed_fps_milli={} inputs={} target_fps={} target_bps={} background={} queue_depth={} gap_p99_us={} acquire_p99_us={} map_p99_us={} encode_p99_us={} dispatch_p99_us={} credit_p99_us={} ws_write_p99_us={} flow_present={} frame_window={} byte_window={} pending_frames={} pending_bytes={} pressure_bytes={} path_delay_us={} oldest_frame_age_us={} delivery_bps={} blocking_flags={} applied_inputs={} input_apply_max_us={} owner_loop_late_p99_us={} acquire_samples={} map_samples={} encode_samples={} flow_samples={} loop_late_samples={} key_frames={} key_bytes={} video_bytes={} repeated_frames={}",
                 identity_.sessionId, diagnostics_.videoGeneration, std::chrono::duration_cast<std::chrono::milliseconds>(now - *cadence_.started).count(), cadence_.fpsMilli(cadence_.received, now), cadence_.fpsMilli(cadence_.written, now), cadence_.fpsMilli(cadence_.displayed, now), cadence_.inputs,
                 diagnostics_.latestTargetFramesPerSecond, diagnostics_.latestTargetBitrate, diagnostics_.latestBackground, diagnostics_.latestBrowserQueueDepth,
                 gap.p99Microseconds, acquire.p99Microseconds, map.p99Microseconds, encode.p99Microseconds, dispatch.p99Microseconds, credit.p99Microseconds, write.p99Microseconds,
                 cadence_.hasFlow, cadence_.hasFlow ? cadence_.flow.frame_window() : 0, cadence_.hasFlow ? cadence_.flow.byte_window() : 0, cadence_.hasFlow ? cadence_.flow.pending_frames() : 0, cadence_.hasFlow ? cadence_.flow.pending_bytes() : 0, cadence_.hasFlow ? cadence_.flow.pressure_bytes() : 0,
                 cadence_.hasFlow ? cadence_.flow.path_delay_microseconds() : 0, cadence_.hasFlow ? cadence_.flow.oldest_frame_age_microseconds() : 0, cadence_.hasFlow ? cadence_.flow.delivery_bytes_per_second() * 8ull : 0, cadence_.blockingFlags,
                 cadence_.appliedInputs, inputApply.maximumMicroseconds, loopLate.p99Microseconds, acquire.samples, map.samples, encode.samples, dispatch.samples, loopLate.samples, cadence_.keyFrames, cadence_.keyBytes, cadence_.videoBytes, cadence_.repeatedFrames);
    cadence_.clear(now);
}
void DesktopSession::close(DesktopCloseReason reason, DesktopCloseOrigin origin) {
    if (closed_) {
        return;
    }
    closed_ = true;
    closeReason_ = reason;
    closeOrigin_ = origin;
    logDiagnostics("desktop_session_end");
    for (auto& endpoint : endpoints_) {
        if (endpoint) {
            endpoint->close();
        }
    }
}
void DesktopSession::logHealth() {
    if (bound_ && ready()) {
        diagnostics_.pipeline.expire();
        logDiagnostics("desktop_session_health");
        diagnostics_.visibleFrames.clear();
    }
}
void DesktopSession::logDiagnostics(std::string_view event) const {
    if (!bound_) {
        return;
    }
    // Fixed names and numeric summaries only; stages overlap and are NOT summed.
    for (std::size_t index = 0; index < kDesktopStageNames.size(); ++index) {
        const auto stage = diagnostics_.pipeline.stages[index].summary();
        spdlog::info("desktop_pipeline_stage session_id={} stage={} samples={} mean_us={} p95_us={} p99_us={} peak_us={} scope=local_only overlap=true end_to_end=false window_capacity=256 diagnostic_dropped={} pending_samples={} browser_requested_contract={} agent_capability_contract={} active_contract={}",
                     identity_.sessionId, kDesktopStageNames[index], stage.samples, stage.meanMicroseconds, stage.p95Microseconds, stage.p99Microseconds, stage.maximumMicroseconds,
                     diagnostics_.pipeline.dropped, diagnostics_.pipeline.pendingSamples(), browserOptimizationRequest_, agentOptimizationCapability_, optimizationActive_ ? 2 : 0);
    }
    spdlog::info("desktop_region_transport session_id={} region_messages={} region_bytes={} copied_pixels={} tile_cache_hits={}",
                 identity_.sessionId, diagnostics_.regionMessages, diagnostics_.regionBytes, diagnostics_.copiedPixels, diagnostics_.tileCacheHits);
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
    const auto encoding = diagnostics_.encoding.summary();
    const auto rafAge = diagnostics_.renderReportedAt == std::chrono::steady_clock::time_point{} ? -1 : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - diagnostics_.renderReportedAt).count();
    const auto encoder = !encoding.samples ? "unknown" : diagnostics_.latestHardwareEncoding ? "hardware"
                                                                                             : "software";
    spdlog::info("{} session_id={} device_id={} reason={} origin={} ready={} duration_ms={} messages={} bytes={} frames={} inputs={} feedback={} backpressure_events={} backpressure_ms={} pending_bytes={} peak_pending_bytes={} max_decode_us={} max_browser_queue_depth={} latest_browser_queue_depth={} background={} video_width={} video_height={} encoder={} encode_samples={} mean_encode_us={} p95_encode_us={} p99_encode_us={} max_encode_us={} last_keyframe_encode_us={} target_fps={} target_bps={}",
                 event, identity_.sessionId, identity_.deviceId, closed_ ? closeReasonName(closeReason_) : "active", closed_ ? closeOriginName(closeOrigin_) : "server", ready_, duration,
                 diagnostics_.forwardedMessages, diagnostics_.forwardedBytes, diagnostics_.videoFrames, diagnostics_.inputEvents,
                 diagnostics_.feedbackMessages, diagnostics_.backpressureEvents, diagnostics_.backpressureMilliseconds,
                 local_->used, diagnostics_.peakPendingBytes, diagnostics_.maximumDecodeMicroseconds,
                 diagnostics_.maximumBrowserQueueDepth, diagnostics_.latestBrowserQueueDepth, diagnostics_.latestBackground,
                 diagnostics_.videoWidth, diagnostics_.videoHeight, encoder, encoding.samples, encoding.meanMicroseconds,
                 encoding.p95Microseconds, encoding.p99Microseconds, encoding.maximumMicroseconds, diagnostics_.latestKeyFrameEncodeMicroseconds,
                 diagnostics_.latestTargetFramesPerSecond, diagnostics_.latestTargetBitrate);
    const auto& raf = diagnostics_.visibleFrames;
    const auto refinementAck = diagnostics_.refinementAcknowledgement.summary();
    spdlog::info("desktop_browser_render session_id={} device_id={} reports={} raf_samples={} raf_p95_us={} raf_p99_us={} raf_ge100ms={} raf_fps_milli={} raf_min_fps_milli={} raf_max_fps_milli={} raf_max_p95_us={} raf_max_p99_us={} raf_max_long_gaps={} report_age_ms={} background={} gpu_encoding={} refinement_messages={} refinement_bytes={} refinement_applied={} refinement_superseded={} refinement_ack_samples={} refinement_ack_p99_us={} refinement_ack_max_us={}",
                 identity_.sessionId, identity_.deviceId, raf.reports, raf.latestSamples, raf.latestP95Microseconds, raf.latestP99Microseconds, raf.latestLongGaps,
                 raf.latestFpsMilli, raf.minimumFpsMilli, raf.maximumFpsMilli, raf.maximumP95Microseconds, raf.maximumP99Microseconds, raf.maximumLongGaps,
                 rafAge, diagnostics_.latestBackground, diagnostics_.latestGpuEncoding, diagnostics_.refinementMessages, diagnostics_.refinementBytes,
                 diagnostics_.refinementApplied, diagnostics_.refinementSuperseded, refinementAck.samples, refinementAck.p99Microseconds, refinementAck.maximumMicroseconds);
}
} // namespace ultraterimal::server
