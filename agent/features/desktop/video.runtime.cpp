#include "video.runtime.h"

#include <algorithm>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <future>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include <ruvia/core/blocking_pool.h>
#include <ruvia/core/event_loop.h>

#include "frame_flow.runtime.h"
#include "platform/windows/deadline_timer.transport.h"
#include "ultraterimal/protocol/desktop_session.protocol.h"
#include "video_worker.runtime.h"

namespace ultraterimal::agent {
using VideoClock = std::chrono::steady_clock;
struct DesktopVideoRuntime::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    ruvia::blocking_pool& pool;
    std::shared_ptr<DesktopVideoWorker> worker = std::make_shared<DesktopVideoWorker>();
    WindowsDeadlineTimer pace;
    WindowsDeadlineTimer releaseWait;
    std::shared_future<std::pair<DesktopVideoDiagnostics, bool>> releaseCompletion;
    std::chrono::steady_clock::time_point nextFrameAt{};
    std::uint32_t display;
    const std::uint32_t serverOptimizationContract;
    const bool serverContinuousFrames;
    std::uint32_t pendingOptimizationContract;
    std::optional<VideoClock::time_point> creditClosedAt;
    std::uint32_t lastCreditWaitMicroseconds = 0, lastBlockingFlags = 0;
    bool stopped = false, pending = false;
    bool reset = false;
    bool baselineRequired = true;
    bool baselineRefresh = false;
    wire::DesktopVideoCapabilities capabilities;
    DesktopFrameFlow flow;
    DesktopVideoDiagnostics diagnostics;
    std::uint64_t publishedVideoFrames = 0, rejectedPrivateVideoFrames = 0, rejectedPrivateJobs = 0;

    void takeWorkerDiagnostics(DesktopVideoDiagnostics snapshot) {
        snapshot.publishedVideoFrames = publishedVideoFrames;
        snapshot.rejectedPrivateVideoFrames = rejectedPrivateVideoFrames;
        snapshot.rejectedPrivateJobs = rejectedPrivateJobs;
        snapshot.ownerRevision = worker->requestedRevision.load();
        diagnostics = std::move(snapshot);
    }
    void rejectPrivate(const std::optional<DesktopVideoUpdate>& result, bool& counted) {
        if (result && !result->frame.data().empty()) {
            // Durable existing reset path: suspend/release must not clear it.
            // The next worker dispatch must rebuild any rejected codec baseline.
            reset = true;
            baselineRequired = true;
        }
        if (counted) {
            return;
        }
        counted = true;
        ++rejectedPrivateJobs;
        rejectedPrivateVideoFrames += result && !result->frame.data().empty();
        diagnostics.rejectedPrivateJobs = rejectedPrivateJobs;
        diagnostics.rejectedPrivateVideoFrames = rejectedPrivateVideoFrames;
    }
    State(ruvia::event_loop owner, ruvia::blocking_pool& blocking, std::uint32_t index, std::uint32_t serverContract, bool continuous) : loop(std::move(owner)), pool(blocking), pace(loop.executor()), releaseWait(loop.executor()), display(index), serverOptimizationContract(serverContract == 2 ? 2 : 0), serverContinuousFrames(continuous && serverContract == 2), pendingOptimizationContract(serverOptimizationContract) {
        if (display >= 64) {
            throw std::invalid_argument("Desktop display index exceeds limit");
        }
    }
    ~State() {
        if (releaseCompletion.valid()) {
            releaseCompletion.wait(); // Shutdown only, never the event-loop hot path.
        }
        // A final owned job releases the thread-affine capture/codec objects;
        // join ensures no encoding worker survives the runtime.
        std::promise<void> released;
        auto completion = released.get_future();
        if (pool.submit([owned = std::move(worker), released = std::move(released)]() mutable { owned.reset(); released.set_value(); }) != ruvia::blocking_submit_status::accepted) {
            std::terminate();
        }
        // join() stops the pool and can discard queued jobs, so the final
        // resource-release job must finish first.
        completion.get();
    }
    void releaseCapture() {
        if (releaseCompletion.valid() && releaseCompletion.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            return;
        }
        std::promise<std::pair<DesktopVideoDiagnostics, bool>> released;
        releaseCompletion = released.get_future().share();
        if (pool.submit([owned = worker, released = std::move(released), owner = weak_from_this(), executor = loop.executor()]() mutable {
                owned->discard();
                released.set_value({owned->diagnostics, !owned->encoder});
                asio::post(executor, [owner] {
                    if (const auto self = owner.lock()) {
                        self->releaseWait.cancel();
                    }
                });
            }) != ruvia::blocking_submit_status::accepted) {
            throw std::logic_error("Desktop timer release could not be queued");
        }
    }
    static asio::awaitable<void> awaitCaptureRelease(std::shared_ptr<State> self) {
        while (self->releaseCompletion.valid() && self->releaseCompletion.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            self->releaseWait.expiresAt(VideoClock::time_point::max());
            std::error_code error;
            co_await self->releaseWait.asyncWait(asio::redirect_error(asio::use_awaitable, error));
            if (error && error != asio::error::operation_aborted) {
                throw std::system_error(error);
            }
        }
        if (self->releaseCompletion.valid()) {
            const auto& released = self->releaseCompletion.get();
            self->takeWorkerDiagnostics(released.first);
            self->baselineRequired = released.second;
            self->releaseCompletion = {};
        }
    }
    bool requestsOptimization() const {
        return serverOptimizationContract == 2 && pendingOptimizationContract == 2 && capabilities.desktop_optimization_contract() == 2;
    }
    bool requestsContinuousFrames() const {
        return serverContinuousFrames && requestsOptimization() && capabilities.continuous_frames();
    }
    static asio::awaitable<std::optional<DesktopVideoUpdate>> nextOwned(std::shared_ptr<State> self, bool refresh) {
        if (self->pending) {
            throw std::logic_error("Concurrent desktop video reads are not allowed");
        }
        struct Pending {
            bool& value;
            ~Pending() { value = false; }
        } guard{self->pending};
        self->pending = true;
        co_await awaitCaptureRelease(self);
        if (self->stopped) {
            co_return std::nullopt;
        }
        if (self->flow.background()) {
            // Hidden workspaces need neither capture nor pixel copies. A
            // visibility ACK wakes this timer immediately; the bounded wait
            // also lets the owner process pending resize/rebind work.
            self->pace.expiresAfter(std::chrono::milliseconds(250));
        } else {
            self->pace.expiresAt(self->nextFrameAt);
        }
        const auto pacingStarted = VideoClock::now();
        const auto plannedWake = !self->flow.background() && self->nextFrameAt != VideoClock::time_point{} ? std::optional(self->nextFrameAt) : std::nullopt;
        std::error_code error;
        co_await self->pace.asyncWait(asio::redirect_error(asio::use_awaitable, error));
        if (self->stopped || self->flow.background()) {
            self->releaseCapture();
            co_await awaitCaptureRelease(self);
            co_return std::nullopt;
        }
        if (error && error != asio::error::operation_aborted) {
            throw std::system_error(error);
        }
        const auto now = std::chrono::steady_clock::now();
        const bool allowEncode = self->reset || self->baselineRequired || self->flow.canEncode(now);
        const bool baselineRefresh = std::exchange(self->baselineRefresh, false);
        refresh = refresh || baselineRefresh;
        const auto [flowWidth, flowHeight] = self->flow.geometry();
        const bool requestedV2 = self->requestsOptimization();
        const bool requestedContinuous = self->requestsContinuousFrames();
        const auto rate = self->flow.rate();
        DesktopVideoStages initialStages;
        initialStages.pacingCreditWait = DesktopVideoStages::microseconds(pacingStarted, now);
        if (!allowEncode && !self->creditClosedAt) {
            self->creditClosedAt = now;
        }
        if (!allowEncode) {
            self->lastBlockingFlags |= self->flow.snapshot(now).blockingFlags;
        }
        if (allowEncode && self->creditClosedAt) {
            initialStages.pacingCreditWait = DesktopVideoStages::microseconds(*self->creditClosedAt, now);
            self->lastCreditWaitMicroseconds = *initialStages.pacingCreditWait;
            self->creditClosedAt.reset();
        }
        const auto queuedAt = VideoClock::now();
        const auto revision = self->worker->requestedRevision.load();
        auto operation = ruvia::run_blocking(self->loop.worker(), [owned = self->worker, display = self->display, refresh, reset = std::exchange(self->reset, false), rate, flowWidth, flowHeight, allowEncode, capabilities = self->capabilities, requestedV2, requestedContinuous, revision, initialStages, queuedAt]() mutable {
            const auto computeStarted = VideoClock::now();
            initialStages.workerQueue = DesktopVideoStages::microseconds(queuedAt, computeStarted);
            std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged;
            auto update = owned->next(display, refresh, reset, rate, flowWidth, flowHeight, geometryChanged, allowEncode, capabilities, requestedV2, requestedContinuous, revision, initialStages);
            const auto completedAt = VideoClock::now();
            if (update) {
                update->workerDispatchMicroseconds = DesktopVideoStages::microseconds(computeStarted, completedAt);
            }
            // A closed credit window still samples cursor and the latest source.
            // ACKs interrupt the bounded owner wait as soon as credit returns.
            const auto wakeAt = allowEncode && owned->pixelsPending && owned->encodeAt > completedAt
                                    ? owned->encodeAt
                                    : completedAt + std::chrono::milliseconds(allowEncode ? 1 : 33);
            return std::tuple{std::move(update), std::max(owned->recovery.nextAttempt(), wakeAt), geometryChanged, owned->diagnostics, !owned->encoder, owned->optimizationV2};
        });
        auto [result, retryAt, geometryChanged, diagnostics, baselineRequired, activeV2] = ((co_await ruvia::as_awaitable(std::move(operation))).take_value());
        self->takeWorkerDiagnostics(diagnostics);
        bool privateRejected = false;
        self->baselineRequired = baselineRequired;
        if (self->stopped || self->flow.background()) {
            self->rejectPrivate(result, privateRejected);
            self->releaseCapture();
            co_await awaitCaptureRelease(self);
            co_return std::nullopt; // Even a foreground transition during release cannot publish this rejected job.
        }
        if (baselineRefresh && (!result || result->frame.data().empty())) {
            self->baselineRefresh = true;
        }
        self->nextFrameAt = retryAt; // Only capture recovery inserts a retry delay.
        if (self->worker->requestedRevision.load() != revision) {
            self->rejectPrivate(result, privateRejected);
            co_return std::nullopt;
        }
        if (self->stopped || self->reset) {
            self->rejectPrivate(result, privateRejected);
            co_return std::nullopt;
        }
        if (geometryChanged) {
            self->rejectPrivate(result, privateRejected);
            if (!self->flow.setGeometry(geometryChanged->first, geometryChanged->second)) {
                throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop dimensions exceed the encoder quality-budget limits");
            }
            self->baselineRequired = true;
            co_return std::nullopt;
        }
        if (result && result->configuration) {
            const bool stillRequested = self->requestsOptimization();
            if (activeV2 != stillRequested) {
                self->rejectPrivate(result, privateRejected);
                self->reset = true;
                self->baselineRequired = true;
                co_return std::nullopt; // Unsent stale Config; rebuild another generation barrier.
            }
        }
        if (result && !result->frame.data().empty()) {
            const auto snapshot = self->flow.snapshot(VideoClock::now());
            auto* sample = &result->flowSample.emplace();
            sample->set_frame_window(snapshot.frameWindow);
            sample->set_byte_window(snapshot.byteWindow);
            sample->set_pending_frames(snapshot.pendingFrames);
            sample->set_pending_bytes(snapshot.pendingBytes);
            sample->set_pressure_bytes(snapshot.pressureBytes);
            sample->set_path_delay_microseconds(snapshot.pathDelayMicroseconds);
            sample->set_oldest_frame_age_microseconds(snapshot.oldestFrameAgeMicroseconds);
            sample->set_delivery_bytes_per_second(snapshot.deliveryBytesPerSecond);
            sample->set_worker_dispatch_microseconds(result->workerDispatchMicroseconds);
            sample->set_credit_wait_microseconds(std::exchange(self->lastCreditWaitMicroseconds, 0));
            sample->set_blocking_flags(snapshot.blockingFlags | std::exchange(self->lastBlockingFlags, 0));
            if (plannedWake && error != asio::error::operation_aborted) {
                sample->set_owner_loop_late_microseconds(DesktopVideoStages::microseconds(*plannedWake, now));
            }
            const auto bytes = result->frame.ByteSizeLong();
            const auto type = result->frame.repeated()    ? (result->frame.key_frame() ? DesktopFrameFlow::FrameType::kRepeatedKeyFrame : DesktopFrameFlow::FrameType::kRepeatedVideo)
                              : result->frame.key_frame() ? DesktopFrameFlow::FrameType::kKeyFrame
                                                          : DesktopFrameFlow::FrameType::kVideo;
            self->flow.setFixedPictureCost(false);
            self->flow.observeFrame(type, bytes, result->changedPixels, std::chrono::microseconds(result->frame.encode_microseconds()));
            self->flow.sent(result->generation, result->frameId, std::chrono::steady_clock::now(), bytes, type);
            self->diagnostics.publishedVideoFrames = ++self->publishedVideoFrames;
        }
        co_return result;
    }
    static asio::awaitable<bool> resizeOwned(std::shared_ptr<State> self, std::uint32_t width, std::uint32_t height) {
        if (self->pending) {
            throw std::logic_error("Concurrent desktop capture operations are not allowed");
        }
        if (self->stopped || width < 16 || height < 16 || width > 4096 || height > 2160 || width % 2 || height % 2) {
            co_return false;
        }
        struct Pending {
            bool& value;
            ~Pending() { value = false; }
        } guard{self->pending};
        self->pending = true;
        auto operation = ruvia::run_blocking(self->loop.worker(), [owned = self->worker, width, height] {
            return owned->resize(width, height);
        });
        const bool applied = ((co_await ruvia::as_awaitable(std::move(operation))).take_value());
        if (applied) {
            self->nextFrameAt = {};
            self->baselineRequired = true;
        }
        co_return applied;
    }
};
DesktopVideoRuntime::DesktopVideoRuntime(ruvia::event_loop loop, ruvia::blocking_pool& pool, std::uint32_t displayIndex, std::uint32_t serverOptimizationContract, bool serverContinuousFrames) : state_(std::make_shared<State>(std::move(loop), pool, displayIndex, serverOptimizationContract, serverContinuousFrames)) {}
DesktopVideoRuntime::~DesktopVideoRuntime() { stop(); }
asio::awaitable<std::optional<DesktopVideoUpdate>> DesktopVideoRuntime::next(bool refresh) { return State::nextOwned(state_, refresh); }
asio::awaitable<bool> DesktopVideoRuntime::resize(std::uint32_t width, std::uint32_t height) { return State::resizeOwned(state_, width, height); }
void DesktopVideoRuntime::stop() {
    if (state_->stopped) {
        return;
    }
    state_->stopped = true;
    ++state_->worker->requestedRevision;
    state_->pace.cancel();
    if (!state_->pending) {
        state_->releaseCapture();
    }
}
void DesktopVideoRuntime::invalidate() {
    state_->reset = true;
    ++state_->worker->requestedRevision;
    state_->pace.cancel();
}
void DesktopVideoRuntime::observeRoundTrip(std::chrono::microseconds elapsed) {
    state_->flow.observeRoundTrip(elapsed, std::chrono::steady_clock::now());
}
DesktopVideoDiagnostics DesktopVideoRuntime::diagnostics() const {
    auto diagnostics = state_->diagnostics;
    diagnostics.ownerRevision = state_->worker->requestedRevision.load();
    return diagnostics;
}
void DesktopVideoRuntime::setOptimizationContract(std::uint32_t contract) {
    const auto permission = contract == 2 ? 2u : 0u;
    if (state_->pendingOptimizationContract == permission) {
        return;
    }
    state_->pendingOptimizationContract = permission;
    state_->baselineRefresh = true;
    state_->baselineRequired = true;
    ++state_->worker->requestedRevision;
    state_->pace.cancel();
}
void DesktopVideoRuntime::capabilities(const wire::DesktopVideoCapabilities& capabilities) {
    const auto signature = [](const wire::DesktopVideoCapabilities& value) {
        unsigned mask = 1u << wire::AVC;
        for (const auto codec : value.codecs()) {
            mask |= 1u << codec;
        }
        return mask;
    };
    const bool sessionChanged = protocol::negotiateDesktopSession(state_->capabilities.session_capabilities()) != protocol::negotiateDesktopSession(capabilities.session_capabilities());
    const bool gateChanged = (state_->capabilities.desktop_optimization_contract() == 2) != (capabilities.desktop_optimization_contract() == 2) && state_->serverOptimizationContract == 2;
    const bool continuousChanged = state_->capabilities.continuous_frames() != capabilities.continuous_frames();
    if (sessionChanged || gateChanged || continuousChanged || signature(state_->capabilities) != signature(capabilities)) {
        state_->baselineRefresh = true;
        state_->baselineRequired = true;
        ++state_->worker->requestedRevision;
    }
    state_->capabilities = capabilities;
    state_->pace.cancel();
}
void DesktopVideoRuntime::feedback(const wire::QualityFeedback& feedback) {
    const bool wasBackground = state_->flow.background();
    const bool hadCredit = state_->flow.canEncode(std::chrono::steady_clock::now());
    if (!state_->flow.acknowledge(feedback.displayed_generation(), feedback.displayed_frame(),
                                  feedback.decode_microseconds(), feedback.queue_depth(), std::chrono::steady_clock::now(), feedback.background(),
                                  feedback.maximum_pending_video_frames())) {
        return;
    }
    if (wasBackground && !state_->flow.background()) {
        state_->baselineRefresh = true;
        state_->pace.cancel();
    }
    if (feedback.background() != wasBackground) {
        ++state_->worker->requestedRevision;
    }
    if (!wasBackground && state_->flow.background()) {
        state_->pace.cancel();
        if (!state_->pending) {
            state_->releaseCapture();
        }
    }
    if (!hadCredit && state_->flow.canEncode(std::chrono::steady_clock::now())) {
        state_->pace.cancel();
    }
}
} // namespace ultraterimal::agent
