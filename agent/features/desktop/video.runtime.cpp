#include "video.runtime.h"

#include <algorithm>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <chrono>
#include <future>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>

#include <ruvia/core/AsioTask.h>
#include <ruvia/core/BlockingPool.h>

#include "damage.protocol.h"
#include "frame_flow.runtime.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/cursor.transport.h"
#include "recovery.runtime.h"
#include "refinement.transport.h"
#include "tile_cache.transport.h"
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
    std::uint64_t generation = 0, frameId = 0, lastPresentTicks = 0;
    std::uint64_t lastEncodedTicks = 0, refinedFrame = 0;
    std::chrono::steady_clock::time_point lastEncodedAt{};
    std::chrono::steady_clock::time_point previousPixelCaptureAt{};
    std::uint32_t width = 0, height = 0;
    bool configurationPending = true;
    bool pixelsPending = false;
    bool readbackReady = false;
    bool capturedOnGpu = false;
    std::chrono::steady_clock::time_point encodeAt{};
    std::chrono::steady_clock::time_point compareAt{};
    DesktopCursor cursor;
    CaptureRecovery recovery;
    DesktopVideoDiagnostics diagnostics;
    DesktopDamage damage;
    DesktopTileCache tiles;
    DesktopRegionBudget regionBudget;
    std::uint32_t damageWidth = 0, damageHeight = 0;
    std::vector<wire::VideoCodec> requestedCodecs{wire::AVC};

    void discard() {
        retained.reset();
        encoder.reset();
        capture.reset();
        cursor = DesktopCursor{};
        configurationPending = true;
        pixelsPending = false;
        lastPresentTicks = 0;
        lastEncodedTicks = refinedFrame = 0;
        lastEncodedAt = {};
        previousPixelCaptureAt = {};
        damage.reset(0, 0);
        damageWidth = damageHeight = 0;
        tiles.clear();
        regionBudget = DesktopRegionBudget{};
        compareAt = {};
    }
    bool materialize(std::span<const wire::Rectangle> regions = {}) {
        try {
            if (regions.empty()) {
                capture->readTexture(*retained);
            } else {
                diagnostics.sparseReadbackPixels += capture->readTextureRegions(*retained, regions);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    bool comparePixels() {
        try {
            if (encoder->gpu()) {
                if (const auto changed = capture->changedTextureTiles()) {
                    damage.filterTiles(*changed);
                }
            } else {
                damage.filterPixels(retained->bgra);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    bool rememberPixels() {
        try {
            if (encoder->gpu()) {
                damage.forgetPixels();
                capture->rememberTexture();
            } else {
                damage.rememberPixels(retained->bgra);
            }
            return true;
        } catch (const DesktopChanged& error) {
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return false;
        }
    }
    std::optional<DesktopVideoUpdate> next(std::uint32_t display, bool refresh, bool reset, DesktopFrameFlow::Rate rate,
                                           std::uint32_t flowWidth, std::uint32_t flowHeight,
                                           std::optional<std::pair<std::uint32_t, std::uint32_t>>& geometryChanged, bool allowEncode, bool allowRefinement, const wire::DesktopVideoCapabilities& capabilities) {
        if (reset) {
            discard();
        }
        std::vector<wire::VideoCodec> codecs{wire::AVC};
        for (const auto codec : capabilities.codecs()) {
            if (codec != wire::AVC) {
                codecs.push_back(static_cast<wire::VideoCodec>(codec));
            }
        }
        const bool codecChanged = codecs != requestedCodecs;
        refresh = refresh || codecChanged;
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
            const auto now = std::chrono::steady_clock::now();
            const bool gpu = encoder && encoder->gpu();
            capturedOnGpu = gpu;
            const bool readPixels = !gpu && allowEncode && (!encoder || configurationPending || refresh || now >= encodeAt);
            const bool readyToEncode = retained && readPixels && (refresh || pixelsPending);
            pixels = capture->nextFrame(std::chrono::milliseconds(readyToEncode || !allowEncode ? 0 : 16), retained ? &retained->bgra : nullptr, nullptr, retained.has_value(), readPixels, !encoder || gpu || codecChanged);
            readbackReady = readPixels || gpu;
            ++diagnostics.captureCalls;
            if (!pixels) {
                ++diagnostics.captureTimeouts;
            } else if (!pixels->captureTicks || pixels->captureTicks == lastPresentTicks) {
                ++diagnostics.cursorOnlyUpdates;
            } else {
                ++diagnostics.pixelUpdates;
                const auto capturedAt = std::chrono::steady_clock::now();
                if (previousPixelCaptureAt != std::chrono::steady_clock::time_point{}) {
                    ++diagnostics.pixelCaptureGaps;
                    diagnostics.pixelCaptureGapsGe45ms += capturedAt - previousPixelCaptureAt >= std::chrono::milliseconds(45);
                }
                previousPixelCaptureAt = capturedAt;
            }
        } catch (const DesktopChanged& error) {
            // Never refresh from a snapshot belonging to the lost desktop.
            discard();
            if (!recovery.restart(std::chrono::steady_clock::now())) {
                throw DesktopFailure(error.code(), "Desktop capture recovery budget exhausted");
            }
            return std::nullopt;
        }
        const auto idle = std::chrono::steady_clock::now() - lastEncodedAt >= std::chrono::milliseconds(180);
        const bool detailDue = allowRefinement && frameId && refinedFrame != frameId && idle;
        if (!pixels && ((!refresh && !pixelsPending && !detailDue) || !retained)) {
            return std::nullopt;
        }
        if (pixels) {
            if (pixels->width != damageWidth || pixels->height != damageHeight) {
                damageWidth = pixels->width;
                damageHeight = pixels->height;
                damage.reset(damageWidth, damageHeight);
            }
            if (pixels->captureTicks && pixels->captureTicks != lastPresentTicks) {
                damage.add(pixels->damageKnown, pixels->moves, pixels->dirty);
                diagnostics.unknownDamageUpdates += !pixels->damageKnown;
            }
            pixelsPending = pixelsPending || (pixels->captureTicks != 0 && pixels->captureTicks != lastEncodedTicks);
            if (pixels->captureTicks != 0) {
                lastPresentTicks = pixels->captureTicks;
            }
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
        if (encoder && encoder->gpu() && ((source.pointerVisible && source.pointerType != 2) || (detailDue && !pixelsPending && !refresh && !configurationPending))) {
            if (!materialize()) {
                return std::nullopt;
            }
        }
        // Fail closed instead of transmitting a rotated or incorrectly cropped
        // desktop. The caller must recreate a supported capture source.
        if (source.rotation != 1 || source.width % 2 || source.height % 2) {
            throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop orientation or dimensions require capture reconfiguration");
        }
        if (source.width != flowWidth || source.height != flowHeight) {
            geometryChanged = std::pair{source.width, source.height};
            return std::nullopt;
        }
        DesktopVideoUpdate update;
        update.desktopLeft = source.desktopLeft;
        update.desktopTop = source.desktopTop;
        update.sourcePresentTicks = lastPresentTicks;
        update.cursor = cursor.update(source, refresh || configurationPending);
        const auto now = std::chrono::steady_clock::now();
        if (detailDue && !pixelsPending && !refresh && !configurationPending) {
            refinedFrame = frameId;
            auto detail = refineVideoDetails(source.bgra, source.width, source.height, &tiles, capabilities.tile_cache());
            if (detail.patches_size()) {
                update.generation = generation;
                update.frameId = frameId;
                update.refinement = std::move(detail);
                return update;
            }
        }
        if (encoder && !configurationPending && ((!pixelsPending && !refresh) || !allowEncode || !readbackReady || (!refresh && now < encodeAt))) {
            if (!allowEncode) {
                ++diagnostics.backpressuredCalls;
            }
            if (!refresh && pixelsPending && now < encodeAt) {
                ++diagnostics.pendingBeforeDeadlineCalls;
            }
            if (update.cursor) {
                return update;
            }
            return std::nullopt;
        }
        if (encoder && !configurationPending && !refresh && frameId && capabilities.region_updates() &&
            (damage.regionCandidate() || now >= compareAt)) {
            const auto previousPixels = damage.changedPixels();
            if (!comparePixels()) {
                return std::nullopt;
            }
            diagnostics.filteredPixels += previousPixels - std::min(previousPixels, damage.changedPixels());
            // Large moving scenes already require video. Reprobe their damage
            // after 100 ms instead of synchronizing a GPU comparison every frame.
            compareAt = damage.unchanged() || damage.regionCandidate() ? now : now + std::chrono::milliseconds(100);
            if (damage.unchanged()) {
                ++diagnostics.unchangedPixelUpdates;
                pixelsPending = false;
                lastEncodedTicks = lastPresentTicks;
                damage.commit();
                return update.cursor ? std::optional{std::move(update)} : std::nullopt;
            }
        }
        update.changedPixels = damage.changedPixels();
        if (encoder && !configurationPending && !refresh && frameId && capabilities.region_updates() && damage.regionCandidate()) {
            ++diagnostics.regionCandidates;
            regionBudget.begin(std::chrono::steady_clock::now(), rate.framesPerSecond);
            const auto patches = damage.patches();
            if (encoder->gpu() && !patches.empty() && !materialize(patches)) {
                return std::nullopt;
            }
            wire::DesktopRegionUpdate region;
            for (const auto& copy : damage.copies()) {
                *region.add_copies() = copy;
            }
            tiles.begin();
            std::size_t bytes = 0;
            bool fits = true;
            // Spend at most half a video-frame budget on PNGs. Cached copies
            // remain cheap; high-entropy updates go directly to the video codec.
            const auto budget = std::min<std::size_t>(256 * 1024, std::max<std::size_t>(4096, rate.bitrate / rate.framesPerSecond / 16));
            for (const auto& area : patches) {
                auto patch = tiles.prepare(source.bgra, source.width, source.height, area, capabilities.tile_cache(), budget - bytes, &regionBudget);
                if (!patch) {
                    fits = false;
                    break;
                }
                bytes += patch->png().size();
                *region.add_patches() = std::move(*patch);
            }
            const bool withinBudget = regionBudget.within(std::chrono::steady_clock::now());
            fits &= withinBudget;
            if (!withinBudget || regionBudget.denied()) {
                ++diagnostics.regionTimeBudgetFailures;
            }
            if (fits) {
                if (frameId == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("Desktop frame sequence exhausted");
                }
                if (!rememberPixels()) {
                    return std::nullopt;
                }
                tiles.commit();
                region.set_encode_microseconds(static_cast<std::uint32_t>(std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now).count())));
                region.set_target_frames_per_second(rate.framesPerSecond);
                region.set_target_bitrate(rate.bitrate);
                update.generation = generation;
                update.baseFrameId = frameId;
                update.frameId = ++frameId;
                update.changedPixels = 0;
                for (const auto& patch : region.patches()) {
                    update.changedPixels += static_cast<std::uint64_t>(patch.destination().width()) * patch.destination().height();
                }
                update.region = std::move(region);
                pixelsPending = false;
                lastEncodedTicks = lastPresentTicks;
                lastEncodedAt = std::chrono::steady_clock::now();
                encodeAt = now + std::chrono::microseconds(1000000 / rate.framesPerSecond);
                damage.commit();
                return update;
            }
            tiles.begin();
            ++diagnostics.regionBudgetFailures;
        }
        if (!encoder || source.width != width || source.height != height || codecChanged) {
            encoder = std::make_unique<VideoEncoder>(VideoEncoderConfig{source.width, source.height, rate.bitrate, rate.framesPerSecond, rate.sharp, codecs}, capture->device());
            requestedCodecs = std::move(codecs);
            width = source.width;
            height = source.height;
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Desktop generation exhausted");
            }
            ++generation;
            frameId = 0;
            refinedFrame = 0;
            configurationPending = true;
            tiles.clear();
        }
        encoder->setRate(rate.framesPerSecond, rate.bitrate);
        encoder->setDetail(rate.sharp);
        if (capturedOnGpu && !encoder->gpu() && !materialize()) {
            return std::nullopt;
        }
        // Schedule from the actual start; missed slots never cause bursts.
        encodeAt = now + std::chrono::microseconds(1000000 / rate.framesPerSecond);
        const auto encodeStarted = std::chrono::steady_clock::now();
        const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(encodeStarted - epoch).count();
        const bool gpuEncoding = encoder->gpu();
        auto encoded = gpuEncoding ? encoder->encodeTexture(capture->texture(), static_cast<std::uint64_t>(timestamp), refresh || configurationPending)
                                   : encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(timestamp), refresh || configurationPending);
        if (!encoded && encoded.error() == VideoEncodeError::kGpuUnavailable) {
            if (!materialize()) {
                return std::nullopt;
            }
            encoded = encoder->encode(source.bgra, static_cast<std::size_t>(width) * 4, static_cast<std::uint64_t>(timestamp), true);
        }
        // Include rejected region work and GPU comparison in encoder pressure.
        const auto encodeMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now).count();
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
            refinedFrame = 0;
            configurationPending = true;
            tiles.clear();
            update.cursor = cursor.update(source, true);
        }
        if (frameId == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Desktop frame sequence exhausted");
        }
        if (!rememberPixels()) {
            return std::nullopt;
        }
        update.generation = generation;
        pixelsPending = false;
        update.frameId = ++frameId;
        lastEncodedTicks = lastPresentTicks;
        lastEncodedAt = std::chrono::steady_clock::now();
        damage.commit();
        if (configurationPending) {
            update.configuration.emplace();
            update.configuration->set_codec(encoded->codec);
            update.configuration->set_desktop_optimization(true);
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
        update.frame.set_gpu_encoding(gpuEncoding && encoder->gpu());
        update.frame.set_target_frames_per_second(rate.framesPerSecond);
        update.frame.set_target_bitrate(rate.bitrate);
        ++diagnostics.encodedFrames;
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
    bool baselineRequired = true;
    bool qualityRefresh = false;
    bool refinementEnabled = false;
    wire::DesktopVideoCapabilities capabilities;
    DesktopFrameFlow flow;
    DesktopVideoDiagnostics diagnostics;

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
        if (self->flow.background()) {
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
        if (self->flow.background()) {
            co_return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool allowEncode = self->reset || self->baselineRequired || self->flow.canEncode(now);
        const bool qualityRefresh = std::exchange(self->qualityRefresh, false);
        refresh = refresh || qualityRefresh;
        const auto [flowWidth, flowHeight] = self->flow.geometry();
        const bool allowRefinement = self->refinementEnabled && self->flow.detailAllowed() && !self->flow.pendingFrames();
        auto operation = ruvia::runBlocking(self->pool, self->loop.handle(), [owned = self->worker, display = self->display, refresh, reset = std::exchange(self->reset, false), rate = self->flow.rate(), flowWidth, flowHeight, allowEncode, allowRefinement, capabilities = self->capabilities] {
            std::optional<std::pair<std::uint32_t, std::uint32_t>> geometryChanged;
            auto update = owned->next(display, refresh, reset, rate, flowWidth, flowHeight, geometryChanged, allowEncode, allowRefinement, capabilities);
            const auto now = std::chrono::steady_clock::now();
            // Cursor metadata remains responsive without repeatedly mapping
            // pixels during a closed receiver window. ACKs wake this wait.
            const auto wakeAt = allowEncode && owned->encodeAt > now ? std::min(owned->encodeAt, now + std::chrono::milliseconds(33)) : now + std::chrono::milliseconds(allowEncode ? 1 : 33);
            return std::tuple{std::move(update), std::max(owned->recovery.nextAttempt(), wakeAt), geometryChanged, owned->diagnostics, !owned->encoder};
        });
        auto [result, retryAt, geometryChanged, diagnostics, baselineRequired] = co_await ruvia::asAwaitable(std::move(operation));
        self->diagnostics = diagnostics;
        self->baselineRequired = baselineRequired;
        if (qualityRefresh && (!result || result->frame.data().empty())) {
            self->qualityRefresh = true;
        }
        self->nextFrameAt = retryAt; // Only capture recovery inserts a retry delay.
        if (self->stopped || self->reset) {
            co_return std::nullopt;
        }
        if (geometryChanged) {
            if (!self->flow.setGeometry(geometryChanged->first, geometryChanged->second)) {
                throw DesktopFailure(wire::DesktopFault::INVALID_GEOMETRY, "Desktop dimensions exceed the encoder quality-budget limits");
            }
            co_return std::nullopt;
        }
        if (result && (!result->frame.data().empty() || result->region)) {
            const auto bytes = result->region ? result->region->ByteSizeLong() : result->frame.ByteSizeLong();
            self->flow.observeEncode(std::chrono::microseconds(result->region ? result->region->encode_microseconds() : result->frame.encode_microseconds()));
            self->flow.observeScene(bytes, result->changedPixels, result->frame.key_frame());
            self->flow.sent(result->generation, result->frameId, std::chrono::steady_clock::now(), bytes);
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
            self->baselineRequired = true;
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
void DesktopVideoRuntime::invalidate() {
    state_->reset = true;
    state_->pace.cancel();
}
void DesktopVideoRuntime::observeRoundTrip(std::chrono::microseconds elapsed) {
    state_->flow.observeRoundTrip(elapsed, std::chrono::steady_clock::now());
}
DesktopVideoDiagnostics DesktopVideoRuntime::diagnostics() const { return state_->diagnostics; }
void DesktopVideoRuntime::capabilities(const wire::DesktopVideoCapabilities& capabilities) {
    const auto signature = [](const wire::DesktopVideoCapabilities& value) {
        unsigned mask = 1u << wire::AVC;
        for (const auto codec : value.codecs()) {
            mask |= 1u << codec;
        }
        return mask;
    };
    if (signature(state_->capabilities) != signature(capabilities)) {
        state_->qualityRefresh = true;
        state_->baselineRequired = true;
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
        state_->qualityRefresh = true;
        state_->pace.cancel();
    }
    state_->refinementEnabled = feedback.video_refinement();
    if (!hadCredit && state_->flow.canEncode(std::chrono::steady_clock::now())) {
        state_->pace.cancel();
    }
    using Quality = DesktopFrameFlow::Quality;
    switch (feedback.preference()) {
    case wire::QualityFeedback::BALANCED:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kBalanced);
        break;
    case wire::QualityFeedback::SMOOTH:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kSmooth);
        break;
    case wire::QualityFeedback::SHARP:
        state_->qualityRefresh |= state_->flow.setQuality(Quality::kSharp);
        break;
    default:
        break;
    }
}
} // namespace ultraterimal::agent
