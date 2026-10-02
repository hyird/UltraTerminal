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
DesktopEndpoint::DesktopEndpoint(ruvia::WorkerHandle worker, protocol::DesktopChannelBinding binding)
    : validator_(binding), queue_(ruvia::makeChannel<std::unique_ptr<DesktopDelivery>>(worker, {.capacity = binding.purpose == wire::DESKTOP_VIDEO ? 2u : 8u})),
      space_(ruvia::makeChannel<bool>(std::move(worker), {.capacity = 1})) {}
ruvia::Task<std::unique_ptr<DesktopDelivery>> DesktopEndpoint::receive(ruvia::StopToken stop) {
    auto result = co_await queue_.second.receive(std::move(stop));
    if (!result.hasValue()) {
        co_return nullptr;
    }
    auto delivery = std::move(result).takeValue();
    if (waiting_) {
        (void)space_.first.send(true);
    }
    if (closed_) {
        delivery.reset();
        // close() wakes the single writer; release its remaining queue before it exits.
        while ((co_await queue_.second.receive()).hasValue()) {
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
DesktopSession::DesktopSession(ruvia::WorkerHandle worker, std::shared_ptr<DesktopBudget> global)
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
ruvia::Task<bool> DesktopSession::forward(std::shared_ptr<DesktopEndpoint> sender, const wire::Envelope& message, ruvia::StopToken stop) {
    const auto found = std::ranges::find(endpoints_, sender);
    if (!ready() || !sender || found == endpoints_.end()) {
        co_return false;
    }
    const auto slot = static_cast<std::size_t>(found - endpoints_.begin());
    const auto origin = originForSlot(slot);
    if (const auto valid = sender->validator_.accept(message); !valid) {
        close(validationReason(valid.error()), origin);
        co_return false;
    }
    auto bytes = protocol::encodeMessage(message);
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
    delivery->local_ = local_;
    delivery->global_ = global_;
    local_->used += delivery->bytes_.size();
    global_->used += delivery->bytes_.size();
    diagnostics_.peakPendingBytes = std::max(diagnostics_.peakPendingBytes, local_->used);
    const auto target = endpoints_[slot ^ 1];
    while (ready() && !stop.stopRequested()) {
        const auto size = delivery->bytes_.size();
        auto sent = target->queue_.first.send(std::move(delivery));
        if (sent.accepted()) {
            ++diagnostics_.forwardedMessages;
            diagnostics_.forwardedBytes += size;
            diagnostics_.videoFrames += message.has_video_frame() || message.has_full_frame() || message.has_diff_batch();
            diagnostics_.inputEvents += message.has_input_event();
            if (message.has_video_config()) {
                diagnostics_.encoding.clear();
                diagnostics_.videoWidth = message.video_config().width();
                diagnostics_.videoHeight = message.video_config().height();
                diagnostics_.latestKeyFrameEncodeMicroseconds = 0;
                diagnostics_.latestTargetFramesPerSecond = 0;
                diagnostics_.latestTargetBitrate = 0;
                diagnostics_.latestHardwareEncoding = false;
            }
            if (message.has_video_frame()) {
                const auto& frame = message.video_frame();
                diagnostics_.encoding.observe(frame.encode_microseconds());
                diagnostics_.latestHardwareEncoding = frame.hardware_encoding();
                diagnostics_.latestTargetFramesPerSecond = frame.target_frames_per_second();
                diagnostics_.latestTargetBitrate = frame.target_bitrate();
                if (frame.key_frame()) {
                    diagnostics_.latestKeyFrameEncodeMicroseconds = frame.encode_microseconds();
                }
            }
            if (message.has_quality_feedback()) {
                ++diagnostics_.feedbackMessages;
                const auto& feedback = message.quality_feedback();
                diagnostics_.maximumDecodeMicroseconds = std::max(diagnostics_.maximumDecodeMicroseconds, feedback.decode_microseconds());
                diagnostics_.maximumBrowserQueueDepth = std::max(diagnostics_.maximumBrowserQueueDepth, feedback.queue_depth());
                diagnostics_.latestBrowserQueueDepth = feedback.queue_depth();
                diagnostics_.latestBackground = feedback.background();
            }
            co_return true;
        }
        if (sent.status() != ruvia::ChannelSendStatus::kFull) {
            break;
        }
        // Keep ownership and accounting while the downstream writer is busy.
        // Waiting suspends this channel's reader, so no input or frame is lost.
        delivery = std::move(*std::move(sent).takeRejected());
        ++diagnostics_.backpressureEvents;
        const auto waitStarted = std::chrono::steady_clock::now();
        target->waiting_ = true;
        const auto available = co_await target->space_.second.receive(stop);
        target->waiting_ = false;
        diagnostics_.backpressureMilliseconds += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStarted).count();
        if (!available.hasValue()) {
            break;
        }
    }
    close(DesktopCloseReason::kQueueClosed, origin);
    co_return false;
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
void DesktopSession::logHealth() const {
    if (bound_ && ready()) {
        logDiagnostics("desktop_session_health");
    }
}
void DesktopSession::logDiagnostics(std::string_view event) const {
    if (!bound_) {
        return;
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
    const auto encoding = diagnostics_.encoding.summary();
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
}
} // namespace ultraterimal::server
