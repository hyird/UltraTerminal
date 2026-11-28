#include "video.runtime.h"

#include <algorithm>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio_bridge.h>
#include <blocking_pool.h>
#include <chrono>
#include <event_loop.h>
#include <exception>
#include <future>
#include <one_shot.h>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include "frame_flow.runtime.h"
#include "platform/windows/deadline_timer.transport.h"
#include "ultraterimal/protocol/desktop_session.protocol.h"
#include "video_worker.runtime.h"

namespace ultraterimal::agent {
namespace {
using VideoClock = std::chrono::steady_clock;
template <typename T>
struct PoolOutcome {
    std::optional<T> value;
    std::exception_ptr failure;
};
template <typename T>
struct PoolCompletionGuard {
    ruvia::one_shot_completion<PoolOutcome<T>> completion;
    explicit PoolCompletionGuard(ruvia::one_shot_completion<PoolOutcome<T>> target) noexcept : completion(std::move(target)) {}
    PoolCompletionGuard(const PoolCompletionGuard&) = delete;
    PoolCompletionGuard& operator=(const PoolCompletionGuard&) = delete;
    PoolCompletionGuard(PoolCompletionGuard&&) noexcept = default;
    PoolCompletionGuard& operator=(PoolCompletionGuard&&) = delete;
    ~PoolCompletionGuard() {
        if (completion.valid()) {
            PoolOutcome<T> cancelled;
            cancelled.failure = std::make_exception_ptr(std::runtime_error("Desktop worker pool stopped"));
            (void)completion.complete(std::move(cancelled));
        }
    }
    void answer(PoolOutcome<T> outcome) {
        const auto target = std::move(completion);
        (void)target.complete(std::move(outcome));
    }
};
template <typename Fn>
ruvia::one_shot_receiver<PoolOutcome<std::invoke_result_t<Fn&>>> startOnPool(ruvia::worker_ref worker, ruvia::blocking_pool& pool, Fn&& fn) {
    using Result = std::invoke_result_t<Fn&>;
    auto [completion, receiver] = ruvia::make_one_shot<PoolOutcome<Result>>(std::move(worker));
    const auto status = pool.post([guard = PoolCompletionGuard<Result>(std::move(completion)), call = std::forward<Fn>(fn)]() mutable {
        PoolOutcome<Result> outcome;
        try {
            outcome.value.emplace(call());
        } catch (...) {
            outcome.failure = std::current_exception();
        }
        guard.answer(std::move(outcome));
    });
    if (!status.accepted()) {
        throw std::runtime_error("Desktop worker pool rejected operation");
    }
    return std::move(receiver);
}
template <typename T>
ruvia::task<T> awaitPoolReceiver(ruvia::one_shot_receiver<PoolOutcome<T>> receiver) {
    auto waited = co_await receiver.wait();
    if (!waited.has_value()) {
        throw std::runtime_error("Desktop worker loop stopped");
    }
    auto outcome = std::move(waited).take_value();
    if (outcome.failure) {
        std::rethrow_exception(outcome.failure);
    }
    co_return std::move(*outcome.value);
}
} // namespace
struct DesktopVideoRuntime::State : std::enable_shared_from_this<State> {
    ruvia::event_loop loop;
    ruvia::blocking_pool& pool;
    std::shared_ptr<DesktopVideoWorker> worker = std::make_shared<DesktopVideoWorker>();
    std::optional<ruvia::one_shot_receiver<PoolOutcome<PrefetchedCapture>>> pendingPrefetch;
    std::shared_future<void> pendingPrefetchDone;
    std::optional<ruvia::one_shot_receiver<PoolOutcome<std::pair<DesktopVideoDiagnostics, bool>>>> pendingRelease;
    std::shared_future<void> pendingReleaseDone;
    WindowsDeadlineTimer pace;
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
    State(ruvia::event_loop owner, ruvia::blocking_pool& blocking, std::uint32_t index, std::uint32_t serverContract, bool continuous, bool desktopAudio) : loop(std::move(owner)), pool(blocking), pace(loop.executor()), display(index), serverOptimizationContract(serverContract == 2 ? 2 : 0), serverContinuousFrames(continuous && serverContract == 2), pendingOptimizationContract(serverOptimizationContract) {
        if (display >= 64) {
            throw std::invalid_argument("Desktop display index exceeds limit");
        }
        worker->serverDesktopAudio = desktopAudio;
    }
    ~State() {
        pendingPrefetch.reset();
        if (pendingPrefetchDone.valid()) {
            pendingPrefetchDone.wait(); // Shutdown only: wait for bounded 16 ms prefetch before release.
        }
        pendingRelease.reset();
        if (pendingReleaseDone.valid()) {
            pendingReleaseDone.wait(); // Shutdown only, never the event-loop hot path.
        }
        // A final owned job releases the thread-affine capture/codec objects;
        // join ensures no encoding worker survives the runtime.
        std::promise<void> released;
        auto completion = released.get_future();
        if (!pool.post([owned = std::move(worker), released = std::move(released)]() mutable { owned.reset(); released.set_value(); }).accepted()) {
            std::terminate();
        }
        // join() stops the pool and can discard queued jobs, so the final
        // resource-release job must finish first.
        completion.get();
    }
    void releaseCapture() {
        if (pendingRelease) {
            return;
        }
        auto waitPrefetch = std::exchange(pendingPrefetchDone, {});
        pendingPrefetch.reset();
        std::promise<void> doneSignal;
        pendingReleaseDone = doneSignal.get_future().share();
        try {
            pendingRelease.emplace(startOnPool(loop.worker(), pool, [owned = worker, waitPrefetch = std::move(waitPrefetch), doneSignal = std::move(doneSignal)]() mutable {
                struct SignalGuard {
                    std::promise<void>& signal;
                    ~SignalGuard() { signal.set_value(); }
                } guard{doneSignal};
                if (waitPrefetch.valid()) {
                    waitPrefetch.wait();
                }
                owned->discard();
                return std::pair{owned->diagnostics, !owned->encoder};
            }));
        } catch (...) {
            pendingReleaseDone = {};
            throw std::logic_error("Desktop timer release could not be queued");
        }
    }
    static asio::awaitable<std::optional<PrefetchedCapture>> takePrefetch(std::shared_ptr<State> self) {
        if (!self->pendingPrefetch) {
            co_return std::nullopt;
        }
        auto receiver = std::move(*self->pendingPrefetch);
        self->pendingPrefetch.reset();
        auto result = co_await ruvia::as_awaitable(awaitPoolReceiver(std::move(receiver)));
        self->pendingPrefetchDone = {};
        co_return result;
    }
    static asio::awaitable<void> awaitCaptureRelease(std::shared_ptr<State> self) {
        if (!self->pendingRelease) {
            co_return;
        }
        auto receiver = std::move(*self->pendingRelease);
        self->pendingRelease.reset();
        const auto released = co_await ruvia::as_awaitable(awaitPoolReceiver(std::move(receiver)));
        self->pendingReleaseDone = {};
        self->takeWorkerDiagnostics(released.first);
        self->baselineRequired = released.second;
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
            if (self->creditClosedAt && self->flow.canEncode(VideoClock::now())) {
                self->nextFrameAt = {};
            }
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
        const auto revision = self->worker->requestedRevision.load();
        const bool resetNow = std::exchange(self->reset, false);
        std::optional<PrefetchedCapture> prefetched;
        if (self->pendingPrefetch) {
            auto completed = co_await takePrefetch(self);
            if (!resetNow && !self->stopped && !self->flow.background()) {
                prefetched = std::move(completed);
            }
        }
        const auto queuedAt = VideoClock::now();
        auto prepareOperation = ruvia::run_blocking_joined(self->loop.worker(), [owned = self->worker, display = self->display, refresh, reset = resetNow, rate, flowWidth, flowHeight, allowEncode, capabilities = self->capabilities, requestedV2, requestedContinuous, revision, initialStages, queuedAt, prefetched = std::move(prefetched)]() mutable {
            const auto computeStarted = VideoClock::now();
            initialStages.workerQueue = DesktopVideoStages::microseconds(queuedAt, computeStarted);
            return owned->prepare(display, refresh, reset, rate, flowWidth, flowHeight, allowEncode, capabilities, requestedV2, requestedContinuous, revision, initialStages, computeStarted, std::move(prefetched));
        });
        auto step = ((co_await ruvia::as_awaitable(std::move(prepareOperation))).take_value());
        if (step.deferredEncode) {
            auto deferred = std::move(*step.deferredEncode);
            bool preconverted = false;
            const auto convertHeight = deferred.rowEnd > deferred.rowBegin ? deferred.rowEnd - deferred.rowBegin : 0u;
            const auto dirtyPixels = static_cast<std::uint64_t>(deferred.width) * convertHeight;
            const auto poolThreads = self->pool.thread_count();
            const auto sliceCount = dirtyPixels >= 640ull * 360 && poolThreads > 1
                                        ? std::clamp<std::size_t>(poolThreads, 1, std::min<std::size_t>(convertHeight / 2, 32))
                                        : (convertHeight == 0 ? 0u : 1u);
            if (convertHeight == 0) {
                preconverted = true;
            } else if (sliceCount > 1) {
                const auto convertStarted = VideoClock::now();
                std::vector<ruvia::one_shot_receiver<PoolOutcome<bool>>> sliceReceivers;
                sliceReceivers.reserve(sliceCount);
                const auto pairCount = convertHeight / 2u;
                for (std::size_t slot = 0; slot < sliceCount; ++slot) {
                    const auto rowBegin = deferred.rowBegin + static_cast<std::uint32_t>((pairCount * slot / sliceCount) * 2u);
                    const auto rowEnd = deferred.rowBegin + static_cast<std::uint32_t>((pairCount * (slot + 1) / sliceCount) * 2u);
                    sliceReceivers.push_back(startOnPool(self->loop.worker(), self->pool, [owned = self->worker, yuvSlot = deferred.yuvSlot, rowBegin, rowEnd, format = deferred.pixelFormat] {
                        return owned->convertRowSlice(yuvSlot, rowBegin, rowEnd, format);
                    }));
                }
                bool allOk = true;
                for (auto& receiver : sliceReceivers) {
                    if (!(co_await ruvia::as_awaitable(awaitPoolReceiver(std::move(receiver))))) {
                        allOk = false;
                    }
                }
                if (allOk) {
                    const auto elapsed = DesktopVideoStages::microseconds(convertStarted, VideoClock::now());
                    deferred.stages.prepareCpu = std::min(60000000u, deferred.stages.prepareCpu.value_or(0) + elapsed);
                }
                preconverted = allOk;
            }
            // Overlap next frame's DXGI capture + direct I420 conversion into the
            // alternate YUV buffer while OpenH264 encodes the current YUV buffer.
            if (preconverted && deferred.canPrefetchNext && poolThreads > 1 && !self->pendingPrefetch &&
                !self->stopped && !self->reset && !self->flow.background()) {
                const std::size_t nextYuvSlot = 1u - deferred.yuvSlot;
                std::promise<void> doneSignal;
                self->pendingPrefetchDone = doneSignal.get_future().share();
                try {
                    self->pendingPrefetch.emplace(startOnPool(self->loop.worker(), self->pool, [owned = self->worker, nextYuvSlot, width = deferred.width, height = deferred.height, epoch = deferred.sourceEpoch, doneSignal = std::move(doneSignal)]() mutable {
                        struct SignalGuard {
                            std::promise<void>& signal;
                            ~SignalGuard() { signal.set_value(); }
                        } guard{doneSignal};
                        return owned->prefetchCapture(nextYuvSlot, width, height, epoch);
                    }));
                } catch (...) {
                    self->pendingPrefetchDone = {};
                }
            }
            auto encodeOperation = ruvia::run_blocking_joined(self->loop.worker(), [owned = self->worker, deferred = std::move(deferred), preconverted]() mutable {
                return owned->encodeDeferred(std::move(deferred), preconverted);
            });
            step = ((co_await ruvia::as_awaitable(std::move(encodeOperation))).take_value());
        }
        auto result = std::move(step.immediate);
        const auto retryAt = step.retryAt;
        const auto geometryChanged = step.geometryChanged;
        const auto baselineRequired = step.baselineRequired;
        const auto activeV2 = step.activeV2;
        self->takeWorkerDiagnostics(std::move(step.diagnostics));
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
            const auto frameProductionUs = std::max(result->productionMicroseconds, result->frame.encode_microseconds());
            self->flow.observeFrame(type, bytes, result->changedPixels, std::chrono::microseconds(frameProductionUs));
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
        (void)co_await takePrefetch(self);
        co_await awaitCaptureRelease(self);
        auto operation = ruvia::run_blocking_joined(self->loop.worker(), [owned = self->worker, width, height] {
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
DesktopVideoRuntime::DesktopVideoRuntime(ruvia::event_loop loop, ruvia::blocking_pool& pool, std::uint32_t displayIndex, std::uint32_t serverOptimizationContract, bool serverContinuousFrames, bool serverDesktopAudio) : state_(std::make_shared<State>(std::move(loop), pool, displayIndex, serverOptimizationContract, serverContinuousFrames, serverDesktopAudio)) {}
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
