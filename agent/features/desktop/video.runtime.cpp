#include "video.runtime.h"

#include <algorithm>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/BlockingPool.h>

#include "pacing.runtime.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/cursor.transport.h"
#include "recovery.runtime.h"
#include "video.transport.h"

namespace ultraterimal::agent {
namespace {
// Accessed exclusively by the single encoding worker, including construction
// and destruction of DXGI/D3D and codec objects.
struct VideoWorker {
    std::unique_ptr<DesktopCapture> capture;
    std::unique_ptr<VideoEncoder> encoder;
    std::optional<CapturedFrame> retained;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::uint64_t generation = 0, frameId = 0;
    std::uint32_t width = 0, height = 0;
    bool configurationPending = true;
    bool pixelsPending = false;
    std::chrono::steady_clock::time_point encodeAt{};
    DesktopCursor cursor;
    CaptureRecovery recovery;

    void discard() {
        retained.reset();
        encoder.reset();
        capture.reset();
        cursor = DesktopCursor{};
        configurationPending = true;
        pixelsPending = false;
    }
    std::optional<DesktopVideoUpdate> next(std::uint32_t display, bool refresh, bool reset, DesktopPacing::Rate rate,
                                           std::uint32_t pacingWidth, std::uint32_t pacingHeight,
                                           std::optional<std::pair<std::uint32_t, std::uint32_t>>& geometryChanged, bool allowEncode) {
        if (reset) {
            discard();
        }
        if (!recovery.ready(std::chrono::steady_clock::now())) {
            return std::nullopt;
        }
        std::optional<CapturedFrame> pixels;
        try {
            if (!capture) {
                capture = std::make_unique<DesktopCapture>(display);
            }
            // DXGI wakes as soon as a desktop or cursor update is available.
            // The bounded wait also lets cancellation and rate changes take effect.
            const bool readyToEncode = retained && allowEncode &&
                                       (refresh || (pixelsPending && std::chrono::steady_clock::now() >= encodeAt));
            pixels = capture->nextFrame(std::chrono::milliseconds(readyToEncode ? 0 : 8), retained ? &retained->bgra : nullptr);
        } catch (const DesktopChanged& error) {
            // Never refresh from a snapshot belonging to the lost desktop.
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return std::nullopt;
        }
        if (!pixels && ((!refresh && !pixelsPending) || !retained)) {
            return std::nullopt;
        }
        if (pixels) {
            pixelsPending = pixelsPending || pixels->captureTicks != 0;
            if (pixels->bgra.empty()) {
                if (!retained || retained->width != pixels->width || retained->height != pixels->height ||
                    retained->rotation != pixels->rotation || retained->desktopLeft != pixels->desktopLeft || retained->desktopTop != pixels->desktopTop) {
                    discard();
                    return std::nullopt;
                }
                pixels->bgra = std::move(retained->bgra);
            }
            retained = std::move(pixels);
        }
        const auto& source = *retained;
        // Fail closed instead of transmitting a rotated or incorrectly cropped
        // desktop. The caller must recreate a supported capture source.
        if (source.rotation != 1 || source.width % 2 || source.height % 2) {
            throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop orientation or dimensions require capture reconfiguration");
        }
        if (source.width != pacingWidth || source.height != pacingHeight) {
            geometryChanged = std::pair{source.width, source.height};
            return std::nullopt;
        }
        DesktopVideoUpdate update;
        update.desktopLeft = source.desktopLeft;
        update.desktopTop = source.desktopTop;
        update.cursor = cursor.update(source, refresh || configurationPending);
        const auto now = std::chrono::steady_clock::now();
        if (encoder && !configurationPending && ((!pixelsPending && !refresh) || !allowEncode || (!refresh && now < encodeAt))) {
            if (update.cursor) {
                return update;
            }
            return std::nullopt;
        }
        if (!encoder || source.width != width || source.height != height) {
            encoder = std::make_unique<VideoEncoder>(VideoEncoderConfig{source.width, source.height, rate.bitrate, rate.framesPerSecond, rate.sharp});
            width = source.width;
            height = source.height;
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Desktop generation exhausted");
            }
            ++generation;
            frameId = 0;
            configurationPending = true;
        }
        encoder->setRate(rate.framesPerSecond, rate.bitrate);
        encoder->setDetail(rate.sharp);
        encodeAt = now + std::chrono::microseconds(1000000 / rate.framesPerSecond);
        const auto encodeStarted = std::chrono::steady_clock::now();
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(encodeStarted - epoch).count();
        auto encoded = encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(timestamp), refresh || configurationPending);
        const auto encodeMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - encodeStarted).count();
        if (!encoded) {
            if (encoded.error() == VideoEncodeError::kFrameSkipped) {
                if (update.cursor) {
                    return update;
                }
                return std::nullopt;
            }
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop video encoding failed");
        }
        if ((refresh || configurationPending) && !encoded->keyFrame) {
            throw DesktopFailure(wire::DesktopFault::ENCODE_FAILED, "Desktop refresh did not produce a key frame");
        }
        if (encoded->baselineReset) {
            if (!encoded->keyFrame || generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("Cannot rebuild desktop encoder baseline");
            }
            ++generation;
            frameId = 0;
            configurationPending = true;
            update.cursor = cursor.update(source, true);
        }
        if (frameId == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop frame sequence exhausted");
        }
        update.generation = generation;
        pixelsPending = false;
        update.frameId = ++frameId;
        if (configurationPending) {
            update.configuration.emplace();
            update.configuration->set_codec(encoded->codec);
            update.configuration->set_width(width);
            update.configuration->set_height(height);
            for (const auto& mode : capture->displayModes()) {
                auto* advertised = update.configuration->add_display_modes();
                advertised->set_width(mode.width);
                advertised->set_height(mode.height);
            }
            configurationPending = false;
        }
        update.frame.set_data(encoded->annexB.data(), encoded->annexB.size());
        update.frame.set_key_frame(encoded->keyFrame);
        update.frame.set_timestamp_microseconds(encoded->timestampMicroseconds);
        update.frame.set_encode_microseconds(static_cast<std::uint32_t>(std::clamp<std::int64_t>(encodeMicroseconds, 1, std::numeric_limits<std::uint32_t>::max())));
        update.frame.set_hardware_encoding(encoder->hardware());
        return update;
    }
};
} // namespace
struct DesktopVideoRuntime::State {
    ruvia::EventLoop loop;
    ruvia::BlockingPool pool{{.threadCount = 1, .queueCapacity = 1}};
    std::shared_ptr<VideoWorker> worker = std::make_shared<VideoWorker>();
    asio::steady_timer pace;
    std::chrono::steady_clock::time_point nextFrameAt{};
    std::uint32_t display;
    bool stopped = false, pending = false;
    bool reset = false;
    bool qualityRefresh = false;
    DesktopPacing pacing;

    State(ruvia::EventLoop owner, std::uint32_t index) : loop(std::move(owner)), pace(loop.executor()), display(index) {
        if (display >= 64) {
            throw std::invalid_argument("Desktop display index exceeds limit");
        }
    }
    ~State() {
        // A final owned job releases the thread-affine capture/codec objects;
        // join ensures no encoding worker survives the runtime.
        std::promise<void> released;
        auto completion = released.get_future();
        if (pool.submit([owned = std::move(worker), released = std::move(released)]() mutable { owned.reset(); released.set_value(); }) != ruvia::BlockingSubmitStatus::kAccepted) {
            std::terminate();
        }
        // join() stops the pool and can discard queued jobs, so the final
        // resource-release job must finish first.
        completion.get();
        pool.join();
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
        if (self->stopped) {
            co_return std::nullopt;
        }
        if (self->pacing.background()) {
            // Hidden workspaces need neither capture nor pixel copies. A
            // visibility ACK wakes this timer immediately; the bounded wait
            // also lets the owner process pending resize/rebind work.
            self->pace.expires_after(std::chrono::milliseconds(250));
        } else {
            self->pace.expires_at(self->nextFrameAt);
        }
        std::error_code error;
        co_await self->pace.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (self->stopped) {
            co_return std::nullopt;
        }
        if (error && error != asio::error::operation_aborted) {
            throw std::system_error(error);
        }
        if (self->pacing.background()) {
            co_return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool allowEncode = self->pacing.canEncode(now);
        const bool qualityRefresh = std::exchange(self->qualityRefresh, false);
        refresh = refresh || qualityRefresh;
        const auto [pacingWidth, pacingHeight] = self->pacing.geometry();
        auto operation = ruvia::runBlocking(self->pool, self->loop.handle(), [owned = self->worker, display = self->display, refresh, reset = std::exchange(self->reset, false), rate = self->pacing.rate(), pacingWidth, pacingHeight, allowEncode] {
            std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged;
            auto update = owned->next(display, refresh, reset, rate, pacingWidth, pacingHeight, geometryChanged, allowEncode);
            return std::tuple{std::move(update), owned->recovery.nextAttempt(), geometryChanged};
        });
        auto [result, retryAt, geometryChanged] = co_await ruvia::asAwaitable(std::move(operation));
        if (qualityRefresh && (!result || result->frame.data().empty())) {
            self->qualityRefresh = true;
        }
        self->nextFrameAt = retryAt; // Only capture recovery inserts a retry delay.
        if (self->stopped || self->reset) {
            co_return std::nullopt;
        }
        if (geometryChanged) {
            if (!self->pacing.setGeometry(geometryChanged->first, geometryChanged->second)) {
                throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop dimensions exceed the encoder quality-budget limits");
            }
            co_return std::nullopt;
        }
        if (result && !result->frame.data().empty()) {
            self->pacing.sent(result->generation, result->frameId, std::chrono::steady_clock::now(), result->frame.ByteSizeLong());
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
        auto operation = ruvia::runBlocking(self->pool, self->loop.handle(), [owned = self->worker, width, height] {
            try {
                if (!owned->capture || !owned->capture->resize(width, height)) {
                    return false;
                }
                owned->discard();
                // An accepted user-requested mode change starts a new recovery
                // episode. Successful frames alone never reset a flapping
                // device's budget, but earlier modes must not penalize this one.
                owned->recovery = CaptureRecovery{};
                return true;
            } catch (const DesktopChanged&) {
                owned->discard();
                return false;
            }
        });
        const bool applied = co_await ruvia::asAwaitable(std::move(operation));
        if (applied) {
            self->nextFrameAt = {};
        }
        co_return applied;
    }
};
DesktopVideoRuntime::DesktopVideoRuntime(ruvia::EventLoop loop, std::uint32_t displayIndex) : state_(std::make_shared<State>(std::move(loop), displayIndex)) {}
DesktopVideoRuntime::~DesktopVideoRuntime() { stop(); }
asio::awaitable<std::optional<DesktopVideoUpdate>> DesktopVideoRuntime::next(bool refresh) { return State::nextOwned(state_, refresh); }
asio::awaitable<bool> DesktopVideoRuntime::resize(std::uint32_t width, std::uint32_t height) { return State::resizeOwned(state_, width, height); }
void DesktopVideoRuntime::stop() {
    state_->stopped = true;
    state_->pace.cancel();
}
void DesktopVideoRuntime::invalidate() { state_->reset = true; }
void DesktopVideoRuntime::observeRoundTrip(std::chrono::microseconds elapsed) {
    state_->pacing.observeRoundTrip(elapsed, std::chrono::steady_clock::now());
}
void DesktopVideoRuntime::feedback(const wire::QualityFeedback& feedback) {
    const bool wasBackground = state_->pacing.background();
    if (!state_->pacing.acknowledge(feedback.displayed_generation(), feedback.displayed_frame(),
                                    feedback.decode_microseconds(), feedback.queue_depth(), std::chrono::steady_clock::now(), feedback.background())) {
        return;
    }
    if (wasBackground && !state_->pacing.background()) {
        state_->qualityRefresh = true;
        state_->pace.cancel();
    }
    using Quality = DesktopPacing::Quality;
    switch (feedback.preference()) {
    case wire::QualityFeedback::BALANCED:
        state_->qualityRefresh |= state_->pacing.setQuality(Quality::kBalanced);
        break;
    case wire::QualityFeedback::SMOOTH:
        state_->qualityRefresh |= state_->pacing.setQuality(Quality::kSmooth);
        break;
    case wire::QualityFeedback::SHARP:
        state_->qualityRefresh |= state_->pacing.setQuality(Quality::kSharp);
        break;
    default:
        break;
    }
}
} // namespace ultraterimal::agent
