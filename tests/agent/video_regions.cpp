#include <algorithm>
#include <array>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <wincodec.h>

// timeapi.h requires Windows SDK types; preserve this dependency order.
// clang-format off
#include <windows.h>
#include <timeapi.h>
// clang-format on

#include <asio/use_future.hpp>

#include <ruvia/core/asio_task.h>
#include <ruvia/core/execution_domain.h>
#include <wrl/client.h>

#include "features/desktop/optimization.runtime.h"
#include "features/desktop/region_budget.types.h"
#include "features/desktop/software_video.transport.h"
#include "features/desktop/video.runtime.h"
#include "features/desktop/video.transport.h"
#include "platform/windows/capture.transport.h"
#include "platform/windows/image_encoder.transport.h"
#include "ultraterimal/protocol/desktop.protocol.h"

namespace fixture {
using Clock = std::chrono::steady_clock;
std::atomic<bool> details{false}, smallDamage{false}, unavailable{false}, stale{false}, stalled{false};
std::atomic<bool> tinyCpuCost{false}, tinyFreshPalette{false};
// Only the progressive GPU-style mechanism case configures this boundary.
// Each encoder snapshots it; all other cases retain their real x264 costs.
std::atomic<bool> knownCheapStatic{false}, knownTinyCodec{false}, zeroInitialPresent{false};
std::atomic<bool> fusedComparison{false}, repeatRgb{false}, trueRgbChange{false}, baselineUnavailable{false}, comparisonUnknown{false}, moveCounter{false}, privateBaselineGate{false};
std::atomic<unsigned> baselineCopies{0}, baselineClears{0}, fusedSubmits{0}, comparisonFault{0}, moveMode{0};
std::atomic<bool> opaqueRgb{false}, cacheShift{false}, privateAuxGate{false}, targetPrivateRegion{false}, stableText{false};
std::vector<ultraterimal::agent::DesktopReadbackRegion> lastComparisonRegions;
std::atomic<std::uint64_t> encodedRgbHash{0}, baselineRgbHash{0};
std::uint64_t rgbHash(std::span<const std::uint8_t> pixels) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            hash = (hash ^ pixels[offset + channel]) * 1099511628211ull;
        }
    }
    return hash;
}
std::atomic<unsigned> timerFault{0}, timerBegins{0}, timerEnds{0}, liveTimers{0};
constexpr std::size_t kKnownVideoBytes = 48000, kTinyKnownVideoBytes = 12288;
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
// Known-cost mechanism cases prepare their real software surrogate with the
// capture resources, on the same worker. The first VIDEO still encodes the
// actual source as a key frame; codec construction is not a GPU timing model.
thread_local std::unique_ptr<ultraterimal::agent::SoftwareVideoEncoder> preparedKnownCodec;
#endif
static_assert(kTinyKnownVideoBytes * 8 * 60 * 11 / 10 < 8000000);
struct TinyCodecSample {
    std::uint64_t call = 0, sleepUs = 0, codecUs = 0, wallUs = 0, bytes = 0;
    std::uint32_t fps = 0, bitrate = 0;
    bool refresh = false;
};
std::array<TinyCodecSample, 1024> tinyCodecSamples{};
std::size_t tinyCodecCount = 0, tinyCodecDropped = 0;
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
// A bounded fake-codec gate, not a GPU fence/Map wait. It lets the owner
// invalidate an already-produced private AVC frame before publication.
struct PrivateVideoGate {
    std::mutex mutex;
    std::condition_variable condition;
    std::atomic<bool> armed{false}, entered{false};
    bool released = false;
    void open() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_one();
    }
    void wait() {
        std::unique_lock lock(mutex);
        entered = true;
        if (!condition.wait_for(lock, std::chrono::seconds(2), [&] { return released; })) {
            throw std::runtime_error("Private fake-codec gate was not released");
        }
    }
} privateVideoGate;
#endif
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
std::atomic<unsigned> pngCalls{0}, pngFault{0};
std::atomic<bool> costTwoTiles{false}, costEntropy{false};
#endif
std::atomic<int> left{5}, top{7};
std::atomic<bool> advanceOnAcquire{false}, readyDuringEncode{false}, resetDuringEncode{false}, boundedOrdering{false};
std::atomic<unsigned> acquisitions{0}, encodedSource{0}, submittedSource{0};
std::atomic<int> submitReset{0}, completeReset{0};
std::atomic<bool> delayedReset{false};
std::atomic<bool> holdCapture{false}, captureHeld{false}, releaseCapture{false};
std::atomic<unsigned> captureEpoch{42}, resets{0};
std::atomic<unsigned> version{1}, pendingChecks{0}, submitted{0}, checked{0}, fullReads{0}, comparisons{0}, remembered{0};
std::unique_ptr<ultraterimal::protocol::DesktopChannelValidator> validator;
std::uint64_t sequence = 1;
void reset(bool text) {
    tinyCpuCost = false;
    tinyFreshPalette = false;
    tinyCodecCount = tinyCodecDropped = 0;
    knownCheapStatic = false;
    knownTinyCodec = false;
    zeroInitialPresent = false;
    fusedComparison = false;
    repeatRgb = false;
    trueRgbChange = false;
    baselineUnavailable = false;
    comparisonUnknown = false;
    moveCounter = privateBaselineGate = false;
    baselineCopies = baselineClears = fusedSubmits = comparisonFault = moveMode = 0;
    opaqueRgb = cacheShift = privateAuxGate = targetPrivateRegion = stableText = false;
    lastComparisonRegions.clear();
    encodedRgbHash = baselineRgbHash = 0;
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
    pngCalls = 0;
    pngFault = 0;
    costTwoTiles = false;
    costEntropy = false;
#endif
    boundedOrdering = false;
    advanceOnAcquire = false;
    readyDuringEncode = false;
    resetDuringEncode = false;
    acquisitions = 0;
    encodedSource = 0;
    submittedSource = 0;
    details = text;
    smallDamage = false;
    unavailable = false;
    stale = false;
    stalled = false;
    left = 5;
    top = 7;
    delayedReset = false;
    submitReset = 0;
    completeReset = 0;
    resets = 0;
    captureEpoch = 42;
    holdCapture = false;
    captureHeld = false;
    releaseCapture = false;
    version = 1;
    pendingChecks = 0;
    submitted = 0;
    checked = 0;
    fullReads = 0;
    comparisons = 0;
    remembered = 0;
}
std::array<std::uint8_t, 4> pixel(unsigned x, unsigned y, unsigned source) {
    if (stableText) {
        source = 1; // Real existing text pattern, stable RGB across capture serials.
    }
    if (moveCounter) {
        if (x < 128 && y < 64) {
            return {0, 0, 255, 255};
        }
        if (x < 256 && y < 64) {
            return {255, 0, 0, 255};
        }
        return {32, static_cast<std::uint8_t>(100 + (x < 384 && y < 64 ? source : 0)), 77, 255};
    }
    if (repeatRgb) {
        const auto color = x < 128 && y < 64 ? (cacheShift || trueRgbChange ? 208 : 32) : cacheShift && x < 256 && y < 64 ? 208
                                                                                                                          : 255;
        return {static_cast<std::uint8_t>(color), 255, 255, opaqueRgb ? std::uint8_t{255} : static_cast<std::uint8_t>(source)};
    }
    if (!details) {
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
        if (costTwoTiles && x < 256 && y < 64) {
            auto random = (y * 1024 + x) ^ (source * 0x9e3779b9u);
            random ^= random >> 16;
            random *= 0x7feb352du;
            random ^= random >> 15;
            random *= 0x846ca68bu;
            random ^= random >> 16;
            return costEntropy ? std::array<std::uint8_t, 4>{static_cast<std::uint8_t>(random), static_cast<std::uint8_t>(random >> 8), static_cast<std::uint8_t>(random >> 16), 255}
                               : std::array<std::uint8_t, 4>{37, 255, 255, 255};
        }
#endif
        const auto color = x < 128 && y < 64 ? (tinyFreshPalette && source % 4 >= 2 ? (source % 4 == 2 ? 80 : 160) : (source % 2 ? 32 : 208)) : 255;
        return {static_cast<std::uint8_t>(color), 255, 255, 255};
    }
    std::array<std::uint8_t, 4> value{static_cast<std::uint8_t>(x % 128), static_cast<std::uint8_t>(y % 64), 100, 255};
    if (x % 128 == 3 || x % 128 == 127 || y % 64 == 5 || y % 64 == 63) {
        value = {static_cast<std::uint8_t>(170 + source % 50), 50, 16, 255};
    } else if (x % 128 == 4 || y % 64 == 6) {
        value = {120, 75, 55, 255};
    }
    return value;
}
void require(bool value, const char* reason) {
    if (!value) {
        throw std::runtime_error(reason);
    }
}
void injectReset(std::atomic<int>& fault) {
    const auto code = fault.exchange(0);
    if (!code) {
        return;
    }
    if (delayedReset) {
        Sleep(6); // Real elapsed platform failure, not a fabricated timing field.
    }
    ++resets;
    ++captureEpoch;
    ++version;
    ++left; // Same dimensions, genuinely different source/desktop identity.
    throw ultraterimal::agent::CaptureReset(code == 1 ? ultraterimal::wire::DesktopFault::CAPTURE_DEVICE_LOST : ultraterimal::wire::DesktopFault::DESKTOP_UNAVAILABLE);
}
void displayed(ultraterimal::agent::DesktopVideoRuntime& video, const ultraterimal::agent::DesktopVideoUpdate& update, bool refinement, bool acknowledgePrimary = true) {
    ultraterimal::wire::Envelope message;
    message.set_protocol_version(1);
    message.set_session_id("fixture");
    message.set_channel_id("video");
    message.set_generation(update.generation);
    if (update.configuration) {
        message.set_sequence(++sequence);
        *message.mutable_video_config() = *update.configuration;
        require(validator->accept(message).has_value(), "Runtime configuration violated negotiated protocol");
    }
    if (!update.frame.data().empty() || update.region) {
        message.set_sequence(++sequence);
        message.set_frame_id(update.frameId);
        message.set_base_frame_id(update.region ? update.baseFrameId : 0);
        if (update.region) {
            *message.mutable_desktop_region_update() = *update.region;
        } else {
            *message.mutable_video_frame() = update.frame;
        }
        require(validator->accept(message).has_value(), "Runtime video/region violated composed baseline");
    }
    if (update.refinement) {
        message.set_sequence(++sequence);
        message.set_frame_id(update.frameId);
        message.set_base_frame_id(update.frameId);
        *message.mutable_video_refinement() = *update.refinement;
        require(validator->accept(message).has_value(), "Runtime refinement violated negotiated tuple/grid/budget");
    }
    if (!acknowledgePrimary || (update.frame.data().empty() && !update.region)) {
        return;
    }
    ultraterimal::wire::QualityFeedback feedback;
    feedback.set_displayed_generation(update.generation);
    feedback.set_displayed_frame(update.frameId);
    feedback.set_preference(tinyCpuCost ? ultraterimal::wire::QualityFeedback::SMOOTH : ultraterimal::wire::QualityFeedback::SHARP);
    feedback.set_decode_microseconds(1000);
    feedback.set_video_refinement(refinement);
    video.feedback(feedback);
}
ultraterimal::wire::Envelope ack(const ultraterimal::agent::DesktopVideoUpdate& update) {
    ultraterimal::wire::Envelope result;
    result.set_protocol_version(1);
    result.set_session_id("fixture");
    result.set_channel_id("input");
    result.set_sequence(1);
    result.set_generation(update.generation);
    result.set_frame_id(update.frameId);
    result.mutable_desktop_refinement_ack()->set_batch_id(update.refinement->batch_id());
    result.mutable_desktop_refinement_ack()->set_status(ultraterimal::wire::DesktopRefinementAck::APPLIED);
    return result;
}
void verifyPixels(const ultraterimal::wire::VideoRefinement& detail, unsigned source, bool reliable = false) {
    using Microsoft::WRL::ComPtr;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    require(SUCCEEDED(initialized), "Cannot initialize PNG verifier");
    struct Apartment {
        ~Apartment() { CoUninitialize(); }
    } apartment;
    ComPtr<IWICImagingFactory> factory;
    require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "Cannot create PNG verifier");
    std::size_t bytes = 0;
    unsigned pixels = 0;
    require(detail.patches_size() <= 8, "V2 patch count exceeded normal batch");
    for (const auto& patch : detail.patches()) {
        require(!patch.png().empty() && (reliable || patch.cache_slot() == 0), "Pixel verifier requires a real PNG product");
        bytes += patch.png().size();
        pixels += patch.destination().width() * patch.destination().height();
        ComPtr<IWICStream> stream;
        require(SUCCEEDED(factory->CreateStream(&stream)), "Cannot create PNG stream");
        require(SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(patch.png().data())), static_cast<DWORD>(patch.png().size()))), "Cannot open PNG bytes");
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) && SUCCEEDED(decoder->GetFrame(0, &frame)), "Cannot decode detail PNG");
        const auto& area = patch.destination();
        std::vector<std::uint8_t> restored(static_cast<std::size_t>(area.width()) * area.height() * 4);
        require(SUCCEEDED(frame->CopyPixels(nullptr, area.width() * 4, static_cast<UINT>(restored.size()), restored.data())), "Cannot restore detail pixels");
        for (unsigned y = 0; y < area.height(); ++y) {
            for (unsigned x = 0; x < area.width(); ++x) {
                const auto expected = pixel(area.x() + x, area.y() + y, source);
                require(std::equal(expected.begin(), expected.end(), restored.begin() + (static_cast<std::size_t>(y) * area.width() + x) * 4), "Detail pixel/source mismatch");
            }
        }
    }
    require(bytes <= 65536 && pixels <= 65536, "V2 byte/pixel budget exceeded");
}
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
#ifndef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
#error Job-return fixture requires the GPU Runtime fixture boundary
#endif
struct JobReturnGate {
    ruvia::execution_handle loop;
    std::function<void()> control;
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false, completed = false, posted = false;
    std::exception_ptr failure;
    ultraterimal::agent::DesktopVideoDiagnostics prepared;
    std::optional<ultraterimal::agent::DesktopRefinementAdmission> association;
    unsigned patches = 0;
    std::uint64_t pngBytes = 0;
    void open() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_one();
    }
};
std::mutex jobReturnMutex;
std::shared_ptr<JobReturnGate> jobReturnGate;
void armJobReturn(const std::shared_ptr<JobReturnGate>& gate) {
    std::lock_guard lock(jobReturnMutex);
    require(!jobReturnGate, "Overlapping job-return fixture gates");
    jobReturnGate = gate;
}
void releaseJobReturn() {
    std::shared_ptr<JobReturnGate> gate;
    {
        std::lock_guard lock(jobReturnMutex);
        gate = std::exchange(jobReturnGate, nullptr);
    }
    if (gate) {
        gate->open();
    }
}
#endif
} // namespace fixture
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
namespace ultraterimal::agent::detail {
void beforeDesktopJobReturnFixture(const std::optional<DesktopVideoUpdate>& update, const DesktopVideoDiagnostics& diagnostics) {
    // Do not gate capture Ready, a cursor or a PRIMARY+dynamic product. The
    // ORIGINAL completed tuple is retained by runBlocking until this returns.
    if (!update || update->configuration || !update->frame.data().empty() || update->region || !update->refinement ||
        !update->refinement->batch_id() || !update->refinement->patches_size() || !update->refinementAdmission) {
        return;
    }
    std::shared_ptr<fixture::JobReturnGate> gate;
    {
        std::lock_guard lock(fixture::jobReturnMutex);
        gate = std::exchange(fixture::jobReturnGate, nullptr);
    }
    if (!gate) {
        return;
    }
    gate->prepared = diagnostics;
    gate->association = update->refinementAdmission;
    gate->patches = update->refinement->patches_size();
    for (const auto& patch : update->refinement->patches()) {
        fixture::require(!patch.png().empty(), "Final auxiliary gate did not retain a real PNG");
        gate->pngBytes += patch.png().size();
    }
    fixture::require(gate->association->batch == DesktopDetailBatchKey{update->generation, update->frameId, update->refinement->batch_id()} &&
                         gate->association->work.generation == update->generation && gate->association->work.frame == update->frameId && gate->association->work.slice &&
                         gate->association->completedJobId >= gate->association->admission.jobId,
                     "Final auxiliary tuple lost its genuine work/batch/admission association");
    fixture::verifyPixels(*update->refinement, fixture::version);
    const auto posted = gate->loop.post([gate] {
        try {
            gate->control(); // Same public owner loop, never a second loop.
        } catch (...) {
            gate->failure = std::current_exception();
        }
        gate->open();
    });
    gate->posted = posted.accepted();
    if (!gate->posted) {
        gate->open();
        throw std::runtime_error("Bounded job-return owner post was rejected");
    }
    std::unique_lock lock(gate->mutex);
    if (!gate->condition.wait_for(lock, std::chrono::seconds(2), [&] { return gate->released; })) {
        gate->released = true;
        throw std::runtime_error("Final auxiliary owner gate exceeded its deadline");
    }
    gate->completed = true;
    // Even a failed control returns the unchanged tuple through original Ruvia
    // completion; the test reports the failure after joining next().
}
} // namespace ultraterimal::agent::detail
#endif
namespace ultraterimal::agent {
// Exclusive platform boundary for failure/lifetime tests. Successful episodes
// use the real system request, so the Runtime, not a suite-wide timer, owns it.
WindowsTimerResolution::~WindowsTimerResolution() { setRequested(false); }
void WindowsTimerResolution::setRequested(bool requested) noexcept {
    if (requested == status_.requested) {
        return;
    }
    if (!requested) {
        if (status_.active) {
            status_.endResult = timeEndPeriod(status_.periodMilliseconds);
            ++fixture::timerEnds;
            --fixture::liveTimers;
            status_.active = false;
        }
        status_.requested = false;
        return;
    }
    status_.requested = true;
    status_.beginResult.reset();
    status_.endResult.reset();
    TIMECAPS caps{};
    status_.capabilitiesResult = fixture::timerFault == 1 ? TIMERR_NOCANDO : timeGetDevCaps(&caps, sizeof(caps));
    if (*status_.capabilitiesResult != 0) {
        return;
    }
    status_.beginResult = fixture::timerFault == 2 ? TIMERR_NOCANDO : timeBeginPeriod(status_.periodMilliseconds);
    status_.active = *status_.beginResult == 0;
    if (status_.active) {
        ++fixture::timerBegins;
        ++fixture::liveTimers;
    }
}
} // namespace ultraterimal::agent
namespace ultraterimal::agent {
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
// The isolated runner compiles the unchanged production WIC implementation
// with just this symbol renamed. Successful products are genuine WIC PNGs;
// delay/error are explicit test boundary faults, never production codec policy.
std::expected<std::string, HRESULT> encodeDetailPngForCostFixture(DesktopPngEncoder& encoder, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height);
std::expected<std::string, HRESULT> encodeDetailPng(DesktopPngEncoder& encoder, std::span<const std::uint8_t> bgra, std::uint32_t width, std::uint32_t height) {
    ++fixture::pngCalls;
    if (fixture::pngFault == 3) {
        Sleep(25);
        return std::unexpected(E_FAIL);
    }
    auto result = encodeDetailPngForCostFixture(encoder, bgra, width, height);
    if (fixture::pngFault == 2) {
        Sleep(25);
    }
    return result;
}
#endif
// Deterministic capture boundary only. Runtime, WIC, cache, pacing and protocol
// are production implementations. GPU variant additionally doubles only the
// VideoEncoder boundary, backed by the real x264 encoder; no production fork.
struct DesktopCapture::State {
    unsigned source = 0, checks = 0;
    std::uint64_t ticks = 0;
    std::vector<std::uint8_t> pixels;
    std::optional<DesktopReadbackResult> roi;
    std::optional<DesktopReadbackTag> baselineTag;
    std::vector<std::uint8_t> baselinePixels;
    bool retired = false;
    fixture::Clock::time_point submittedAt{};
};
DesktopCapture::DesktopCapture(std::uint32_t) : state_(std::make_unique<State>()) {
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    if (fixture::knownTinyCodec || fixture::knownCheapStatic) {
        fixture::preparedKnownCodec = std::make_unique<SoftwareVideoEncoder>(VideoEncoderConfig{1024, 512, 8000000, 30, false, {wire::AVC}});
    }
#endif
}
DesktopCapture::~DesktopCapture() = default;
std::optional<CapturedFrame> DesktopCapture::nextFrame(std::chrono::milliseconds, std::vector<std::uint8_t>*, CaptureTiming* timing, bool, bool readPixels, bool) {
    if (fixture::holdCapture.exchange(false)) {
        fixture::captureHeld = true;
        const auto until = fixture::Clock::now() + std::chrono::seconds(2);
        while (!fixture::releaseCapture && fixture::Clock::now() < until) {
            Sleep(1);
        }
        fixture::require(fixture::releaseCapture, "Owner did not release held capture job");
    }
    if (timing) {
        timing->stages.acquireWaitMicroseconds = 0;
    }
    ++fixture::acquisitions;
    if (fixture::advanceOnAcquire) {
        ++fixture::version;
    }
    const unsigned source = fixture::details || fixture::boundedOrdering || fixture::tinyCpuCost ? fixture::version.load() : state_->source + 1;
    if (source == state_->source) {
        return std::nullopt;
    }
    state_->source = source;
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    state_->ticks = static_cast<std::uint64_t>(ticks.QuadPart);
    const bool firstSnapshot = state_->pixels.empty(); // A recreated capture has no partial-update baseline, regardless of serial.
    state_->pixels.resize(1024 * 512 * 4);
    for (unsigned y = 0; y < 512; ++y) {
        for (unsigned x = 0; x < 1024; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * 1024 + x) * 4;
            unsigned dirtyWidth = 128;
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
            dirtyWidth = fixture::costTwoTiles ? 256 : 128;
#endif
            const unsigned dirtyLeft = fixture::cacheShift ? 128 : 0;
            if (firstSnapshot || fixture::moveCounter || !fixture::smallDamage || source == 1 || (x >= dirtyLeft && x < dirtyLeft + dirtyWidth && y < 64)) {
                const auto value = fixture::pixel(x, y, source);
                std::copy(value.begin(), value.end(), state_->pixels.begin() + offset);
            }
        }
    }
    CapturedFrame frame;
    frame.width = 1024;
    frame.height = 512;
    frame.rotation = 1;
    frame.desktopLeft = fixture::left;
    frame.desktopTop = fixture::top;
    frame.captureTicks = fixture::zeroInitialPresent && source == 1 ? 0 : state_->ticks;
    frame.captureEpoch = fixture::captureEpoch;
    frame.sourceSerial = source;
    frame.stageTiming.acquireWaitMicroseconds = 0;
    frame.pointerVisible = fixture::fusedComparison;
    frame.pointerX = fixture::fusedComparison ? static_cast<std::int32_t>(source) : 0;
    frame.pointerType = 2;
    if (fixture::fusedComparison) {
        frame.pointerWidth = frame.pointerHeight = 1;
        frame.pointerPitch = 4;
        frame.pointerShape = {0, 0, 0, 255};
    }
    frame.damageKnown = fixture::fusedComparison ? !fixture::comparisonUnknown : fixture::tinyCpuCost || fixture::details || source % 3 != 0;
    wire::Rectangle area;
    unsigned dirtyWidth = 128;
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
    dirtyWidth = fixture::costTwoTiles ? 256 : 128;
#endif
    area.set_width(fixture::smallDamage ? dirtyWidth : 1024);
    area.set_height(fixture::smallDamage ? 64 : 512);
    area.set_x(fixture::cacheShift ? 128 : 0);
    frame.dirty.push_back(area);
    if (fixture::moveCounter && source > 1) {
        frame.dirty.clear();
        area.set_x(128);
        area.set_width(256);
        area.set_height(64);
        frame.dirty.push_back(area);
        auto& copy = frame.moves.emplace_back();
        copy.set_source_x(0);
        copy.set_source_y(0);
        *copy.mutable_destination() = area;
        copy.mutable_destination()->set_width(128);
        if (fixture::moveMode) {
            auto overlap = copy;
            overlap.mutable_destination()->set_x(192); // Overlapping destinations share B/C candidate tiles.
            frame.moves.push_back(overlap);
        }
    }
    const bool cpuKnown = readPixels || fixture::tinyCpuCost; // Both variants explicitly supply this case's complete CPU source.
    if (cpuKnown) {
        frame.bgra = state_->pixels;
        frame.stageTiming.rowCopyMicroseconds = 12;
        if (timing) {
            timing->stages.rowCopyMicroseconds = 12;
        }
    }
    return frame;
}
ID3D11Device* DesktopCapture::device() const { return nullptr; }
ID3D11Texture2D* DesktopCapture::texture() const { return reinterpret_cast<ID3D11Texture2D*>(state_->pixels.data()); }
void DesktopCapture::readTexture(CapturedFrame& frame, CaptureTiming* timing) {
    ++fixture::fullReads;
    frame.bgra = state_->pixels;
    frame.stageTiming.mapMicroseconds = 7;
    frame.stageTiming.rowCopyMicroseconds = 12;
    if (timing) {
        timing->stages = frame.stageTiming;
    }
}
std::uint64_t DesktopCapture::readTextureRegions(CapturedFrame&, std::span<const wire::Rectangle>, CaptureTiming*) { throw std::runtime_error("Unexpected synchronous optional GPU readback"); }
std::optional<std::vector<std::uint32_t>> DesktopCapture::changedTextureTiles() {
    ++fixture::comparisons;
    throw std::runtime_error("Unexpected synchronous GPU comparison");
}
void DesktopCapture::rememberTexture() { ++fixture::remembered; }
std::expected<DesktopReadbackSubmit, HRESULT> DesktopCapture::submitTextureRegions(const DesktopReadbackTag& tag, std::span<const DesktopReadbackRegion> regions) {
    fixture::injectReset(fixture::submitReset);
    if (state_->roi) {
        return DesktopReadbackSubmit::kBusy;
    }
    if (fixture::unavailable) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    fixture::require(tag.captureEpoch == fixture::captureEpoch && tag.sourceSerial == state_->source && tag.width == 1024 && tag.height == 512 && tag.desktopLeft == fixture::left && tag.desktopTop == fixture::top, "Runtime submitted wrong source tag");
    fixture::require(!regions.empty() && regions.size() <= 32, "Runtime submitted an unbounded ROI");
    DesktopReadbackResult result;
    result.tag = tag;
    result.regions.assign(regions.begin(), regions.end());
    for (const auto& area : regions) {
        fixture::require(area.width <= 128 && area.height <= 64 && area.x + area.width <= 1024 && area.y + area.height <= 512, "Runtime requested invalid sparse pixels");
        result.pixels += static_cast<std::uint64_t>(area.width) * area.height;
        for (unsigned y = 0; y < area.height; ++y) {
            const auto offset = (static_cast<std::size_t>(area.y + y) * 1024 + area.x) * 4;
            result.bgra.insert(result.bgra.end(), state_->pixels.begin() + offset, state_->pixels.begin() + offset + area.width * 4);
        }
    }
    state_->roi = std::move(result);
    state_->checks = 0;
    state_->submittedAt = fixture::Clock::now();
    state_->retired = false;
    ++fixture::submitted;
    fixture::submittedSource = state_->source;
    return DesktopReadbackSubmit::kSubmitted;
}
std::expected<DesktopReadbackCompletion, HRESULT> DesktopCapture::tryCompleteTextureRegions(const DesktopReadbackTag& expected) {
    ++fixture::checked;
    if (!state_->roi) {
        return DesktopReadbackCompletion{};
    }
    fixture::injectReset(fixture::completeReset);
    if (state_->roi->tag != expected || expected.sourceSerial != state_->source) {
        state_->retired = true;
    }
    DesktopReadbackCompletion result;
    result.timing.mapMicroseconds = 7;
    if (state_->roi->comparison) {
        result.timing.gpuBytes = (32 + state_->roi->pixels) * 4;
    }
    if (fixture::stalled || (fixture::readyDuringEncode && fixture::encodedSource != state_->roi->tag.sourceSerial) || state_->checks++ < fixture::pendingChecks) {
        result.status = DesktopReadbackStatus::kPending;
        return result;
    }
    if (fixture::boundedOrdering && !fixture::readyDuringEncode) {
        std::cerr << "ordering completion submit_to_ready_us=" << std::chrono::duration_cast<std::chrono::microseconds>(fixture::Clock::now() - state_->submittedAt).count() << '\n';
    }
    result.status = state_->retired ? DesktopReadbackStatus::kRetired : DesktopReadbackStatus::kReady;
    result.timing.copyDispatchMicroseconds = 3;
    result.timing.rowCopyMicroseconds = 11;
    result.timing.readbackReadyMicroseconds = 30;
    if (!state_->retired) {
        result.result = std::move(state_->roi);
        if (fixture::stale) {
            ++result.result->tag.captureEpoch;
        }
    }
    state_->roi.reset();
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    if (fixture::privateAuxGate && result.status == DesktopReadbackStatus::kReady && fixture::privateVideoGate.armed.exchange(false)) {
        fixture::privateVideoGate.wait(); // Owned private auxiliary BGRA, not a fake late Ready.
    }
#endif
    return result;
}
void DesktopCapture::retireTextureRegions() { state_->retired = true; }
std::expected<DesktopTextureBaselineResult, HRESULT> DesktopCapture::rememberTextureBaseline(const DesktopReadbackTag& tag) {
    // Own all new Capture symbols: never pull real Capture's incompatible State
    // into this exclusive boundary. These copies do not count as CPU readback.
    if (!fixture::fusedComparison || fixture::baselineUnavailable) {
        return DesktopTextureBaselineResult{};
    }
    if (state_->roi) {
        return DesktopTextureBaselineResult{DesktopTextureBaselineStatus::kBusy};
    }
    fixture::require(tag.sourceSerial == state_->source && tag.generation && tag.composedBase, "Fused baseline was not the primary source/new composed base");
    state_->baselinePixels = state_->pixels;
    state_->baselineTag = tag;
    fixture::baselineRgbHash = fixture::rgbHash(state_->baselinePixels);
    ++fixture::baselineCopies;
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    const bool targetProduct = !fixture::targetPrivateRegion || fixture::encodedSource != state_->source;
    if (fixture::privateBaselineGate && targetProduct && fixture::privateVideoGate.armed.exchange(false)) {
        fixture::privateVideoGate.wait();
    }
#endif
    return DesktopTextureBaselineResult{DesktopTextureBaselineStatus::kQueued, 0, state_->pixels.size()};
}
void DesktopCapture::clearTextureBaseline() {
    ++fixture::baselineClears;
    state_->baselineTag.reset();
    state_->baselinePixels.clear();
    if (state_->roi && state_->roi->comparison) {
        state_->retired = true;
    }
}
std::expected<DesktopReadbackSubmit, HRESULT> DesktopCapture::submitTextureComparison(const DesktopReadbackTag& current, const DesktopReadbackTag& baseline, std::span<const DesktopReadbackRegion> regions) {
    if (state_->roi) {
        return DesktopReadbackSubmit::kBusy;
    }
    if (!fixture::fusedComparison || state_->baselineTag != baseline) {
        return DesktopReadbackSubmit::kUnavailable;
    }
    const auto submitted = submitTextureRegions(current, regions);
    if (!submitted || *submitted != DesktopReadbackSubmit::kSubmitted) {
        return submitted;
    }
    state_->roi->comparison = DesktopReadbackComparison{baseline, {}};
    for (const auto& area : regions) {
        bool changed = false;
        for (unsigned y = 0; y < area.height; ++y) {
            for (unsigned x = 0; x < area.width; ++x) {
                const auto offset = (static_cast<std::size_t>(area.y + y) * current.width + area.x + x) * 4;
                changed |= !std::equal(state_->pixels.begin() + offset, state_->pixels.begin() + offset + 3, state_->baselinePixels.begin() + offset);
            }
        }
        state_->roi->comparison->changed.push_back(changed);
    }
    fixture::lastComparisonRegions.assign(regions.begin(), regions.end());
    const auto fault = fixture::comparisonFault.load();
    const auto corruptTag = [](DesktopReadbackTag& tag, unsigned field) {
        switch (field) {
        case 0:
            ++tag.captureEpoch;
            break;
        case 1:
            ++tag.sourceSerial;
            break;
        case 2:
            ++tag.generation;
            break;
        case 3:
            ++tag.composedBase;
            break;
        case 4:
            ++tag.width;
            break;
        case 5:
            ++tag.height;
            break;
        case 6:
            ++tag.desktopLeft;
            break;
        case 7:
            ++tag.desktopTop;
            break;
        case 8:
            ++tag.rotation;
            break;
        default:
            fixture::require(false, "Unrecognized tag fault");
        }
    };
    if (fault >= 11 && fault <= 19) {
        corruptTag(state_->roi->comparison->baselineTag, fault - 11);
    } else if (fault >= 20 && fault <= 28) {
        corruptTag(state_->roi->tag, fault - 20);
    }
    switch (fault) {
    case 1:
        ++state_->roi->comparison->baselineTag.composedBase;
        break;
    case 2:
        state_->roi->comparison->changed.clear();
        break;
    case 3:
        ++state_->roi->regions.front().width;
        break;
    case 4:
        state_->roi->bgra.pop_back();
        break;
    case 5:
        state_->roi->comparison->changed.front() = 2;
        break;
    case 29:
        state_->roi->comparison.reset();
        break;
    case 30:
        state_->roi->regions.clear();
        break;
    case 31:
        ++state_->roi->pixels;
        break;
    case 32:
        fixture::require(state_->roi->regions.size() == 2, "Duplicate geometry fault needs the complete B/C union");
        state_->roi->regions[1] = state_->roi->regions[0];
        state_->roi->comparison->changed.assign(2, 0); // A tempting but incomplete all-zero proof.
        break;
    default:
        break;
    }
    ++fixture::fusedSubmits;
    return submitted;
}
std::vector<DesktopCapture::DisplayMode> DesktopCapture::displayModes() const { return {{1024, 512}}; }
bool DesktopCapture::resize(std::uint32_t, std::uint32_t) { return false; }
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
struct VideoEncoder::State {
    VideoEncoderConfig config;
    std::unique_ptr<SoftwareVideoEncoder> software;
    const bool knownCheapStatic = fixture::knownCheapStatic.load();
    const bool knownTinyCodec = fixture::knownTinyCodec.load();
    explicit State(VideoEncoderConfig settings) : config(settings), software(std::move(fixture::preparedKnownCodec)) {
        if (!software) {
            software = std::make_unique<SoftwareVideoEncoder>(settings);
        } else {
            software->setRate(settings.framesPerSecond, settings.bitrate);
            software->setDetail(settings.sharp);
        }
    }
};
VideoEncoder::VideoEncoder(VideoEncoderConfig config, ID3D11Device*) : state_(std::make_unique<State>(std::move(config))) {}
VideoEncoder::~VideoEncoder() = default;
void VideoEncoder::setRate(std::uint32_t fps, std::uint32_t bitrate) {
    state_->config.framesPerSecond = fps;
    state_->config.bitrate = bitrate;
    state_->software->setRate(fps, bitrate);
}
void VideoEncoder::setDetail(bool sharp) { state_->software->setDetail(sharp); }
bool VideoEncoder::hardware() const { return true; }
bool VideoEncoder::gpu() const { return true; }
std::expected<EncodedVideoFrame, VideoEncodeError> VideoEncoder::encodeTexture(ID3D11Texture2D* texture, std::uint64_t timestamp, bool refresh) {
    const auto wallStarted = fixture::Clock::now();
    if (fixture::tinyCpuCost && !state_->knownTinyCodec) {
        // Keep the independent slow boundary real. Sleep(8) overshoot cannot
        // establish a portable 30fps premise; it is not a hardware measurement.
        Sleep(8);
    }
    const auto codecStarted = fixture::Clock::now();
    auto result = state_->software->encode({reinterpret_cast<const std::uint8_t*>(texture), static_cast<std::size_t>(state_->config.width) * state_->config.height * 4}, state_->config.width * 4, timestamp, refresh || fixture::resetDuringEncode);
    if (result && fixture::fusedComparison) {
        fixture::encodedRgbHash = fixture::rgbHash({reinterpret_cast<const std::uint8_t*>(texture), static_cast<std::size_t>(state_->config.width) * state_->config.height * 4});
    }
    const auto codecEnded = fixture::Clock::now();
    if (result && !fixture::privateBaselineGate && fixture::privateVideoGate.armed.exchange(false)) {
        fixture::privateVideoGate.wait();
    }
    if (fixture::tinyCpuCost) {
        const fixture::TinyCodecSample sample{fixture::tinyCodecCount + fixture::tinyCodecDropped + 1,
                                              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(codecStarted - wallStarted).count()),
                                              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(codecEnded - codecStarted).count()),
                                              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(codecEnded - wallStarted).count()),
                                              result ? result->annexB.size() : 0, state_->config.framesPerSecond, state_->config.bitrate, refresh};
        if (fixture::tinyCodecCount < fixture::tinyCodecSamples.size()) {
            fixture::tinyCodecSamples[fixture::tinyCodecCount++] = sample;
        } else {
            ++fixture::tinyCodecDropped;
        }
    }
    const auto knownVideoBytes = state_->knownTinyCodec ? fixture::kTinyKnownVideoBytes : state_->knownCheapStatic ? fixture::kKnownVideoBytes
                                                                                                                   : 0;
    if (result && result->annexB.size() < knownVideoBytes) {
        // Test-only synthetic serialized VIDEO cost, not extra wall work or a
        // throughput claim. Keep real x264 headers/pictures and append one AVC
        // filler-data NAL (type 12, ff filler bytes, rbsp stop bit). WIC/ROI,
        // source order, QoS, absolute deadline and shared probe stay real.
        result->annexB.insert(result->annexB.end(), {0, 0, 1, 12});
        result->annexB.resize(std::max(knownVideoBytes, result->annexB.size() + 1), 255);
        result->annexB.back() = 128;
    }
    fixture::encodedSource = fixture::version.load(); // GPU completion opportunity during the real encode call.
    if (result && fixture::resetDuringEncode) {
        result->baselineReset = true;
    }
    return result;
}
std::expected<EncodedVideoFrame, VideoEncodeError> VideoEncoder::encode(std::span<const std::uint8_t> pixels, std::size_t stride, std::uint64_t timestamp, bool refresh) { return state_->software->encode(pixels, stride, timestamp, refresh); }
#endif
} // namespace ultraterimal::agent
namespace {
using namespace ultraterimal;
using fixture::require;
using Clock = fixture::Clock;
void admissionNumeric(const agent::DesktopAdmissionSnapshot& s) {
    const auto us = [](auto duration) { return std::chrono::duration_cast<std::chrono::microseconds>(duration).count(); };
    std::cerr << "admission_snapshot id=" << s.id << " job=" << s.jobId << " revision=" << s.revision
              << " fps=" << s.framesPerSecond << " bitrate=" << s.bitrate << " allowance_us=" << s.allowance.count()
              << " T_us=" << us(s.started) << " admitted_us=" << us(s.admittedAt) << " D_us=" << us(s.deadline)
              << " window_us=" << s.window.count() << " remaining_us=" << s.remainingAtAdmission.count()
              << " prep_us=" << (s.preparePrediction ? s.preparePrediction->count() : -1) << " bytes=" << s.messageBytes.value_or(0)
              << " bytes_known=" << s.messageBytes.has_value() << " send_us=" << (s.sendPrediction ? s.sendPrediction->count() : -1)
              << " video_encode_us=" << s.videoEncodeEstimate.count() << " video_bytes=" << s.videoBytes << " video_alt_us=" << s.videoAlternative.count()
              << " units=" << s.workUnits << " choice=" << static_cast<unsigned>(s.choice) << " source=" << static_cast<unsigned>(s.source)
              << " region=" << s.region << " fit=" << s.fitDenied << " adv=" << s.advantageDenied << " unknown=" << s.unknownPrediction
              << " epoch=" << s.captureEpoch << " serial=" << s.sourceSerial << " generation=" << s.sourceGeneration << " base=" << s.composedBase
              << " width=" << s.width << " height=" << s.height << " rotation=" << s.rotation << " left=" << s.desktopLeft << " top=" << s.desktopTop << '\n';
}
void readbackDiagnostic(const agent::DesktopVideoDiagnostics& diagnostics) {
    if (!diagnostics.lastReadback) {
        std::cerr << "readback_trace absent=1 pid=" << GetCurrentProcessId() << '\n';
        return;
    }
    const auto& trace = *diagnostics.lastReadback;
    const auto& s = trace.admission;
    const auto ns = [](auto value) { return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count(); };
    std::cerr << "readback_trace_meta work=" << s.id << " purpose=" << trace.purpose << " admission_job=" << s.jobId << " revision=" << s.revision
              << " pid=" << GetCurrentProcessId() << " print_thread=" << GetCurrentThreadId() << " fused_fixture=" << fixture::fusedComparison << " baseline_copies=" << fixture::baselineCopies
              << " fps=" << s.framesPerSecond << " bitrate=" << s.bitrate << " A_us=" << s.allowance.count() << " window_us=" << s.window.count()
              << " T_ns=" << ns(s.started) << " E_ns=" << ns(s.encodeAt) << " D_ns=" << (s.decisionDeadline ? ns(*s.decisionDeadline) : ns(s.deadline)) << " costD_ns=" << ns(s.deadline)
              << " admitted_ns=" << ns(s.admittedAt) << " remaining_admitted_us=" << s.remainingAtAdmission.count()
              << " choice=" << static_cast<unsigned>(s.choice) << " probe_reasons=" << s.probeReasons << " shared_probe_known=" << s.sharedProbeBefore.has_value()
              << " shared_probe_ns=" << (s.sharedProbeBefore ? ns(*s.sharedProbeBefore) : 0)
              << " prep_known=" << s.preparePrediction.has_value() << " prep_us=" << (s.preparePrediction ? s.preparePrediction->count() : -1)
              << " send_known=" << s.sendPrediction.has_value() << " send_us=" << (s.sendPrediction ? s.sendPrediction->count() : -1)
              << " bytes_known=" << s.messageBytes.has_value() << " bytes=" << s.messageBytes.value_or(0) << " prefix_us=" << s.preparationPrefix.count()
              << " epoch=" << s.captureEpoch << " serial=" << s.sourceSerial << " generation=" << s.sourceGeneration << " base=" << s.composedBase
              << " width=" << s.width << " height=" << s.height << " rotation=" << s.rotation << " left=" << s.desktopLeft << " top=" << s.desktopTop
              << " predicted_units=" << s.workUnits << " submitted_tiles=" << trace.submittedTiles << " submitted_pixels=" << trace.submittedPixels << " submitted_units=" << trace.submittedWorkUnits
              << " retire_reason=" << static_cast<unsigned>(trace.retirementReason) << " fallback_reason=" << static_cast<unsigned>(trace.videoFallbackReason)
              << " fallback_job=" << trace.fallbackJobId << " fallback_serial=" << trace.fallbackSourceSerial << " count=" << trace.count << " dropped=" << trace.dropped
              << " acquired_epoch=" << diagnostics.lastAcquireEpoch << " acquired_serial=" << diagnostics.lastAcquireSerial << " acquired_present=" << diagnostics.lastAcquirePresentTicks << '\n';
    if (trace.comparisonBaseline) {
        const auto& b = *trace.comparisonBaseline;
        std::cerr << "readback_baseline work=" << s.id << " epoch=" << b.captureEpoch << " serial=" << b.sourceSerial << " generation=" << b.generation << " base=" << b.composedBase
                  << " width=" << b.width << " height=" << b.height << " rotation=" << b.rotation << " left=" << b.desktopLeft << " top=" << b.desktopTop << '\n';
    }
    if (trace.png) {
        const auto& png = *trace.png;
        std::cerr << "comparison_png work=" << s.id << " cooldown_known=" << png.cooldownUntil.has_value() << " cooldown_ns=" << (png.cooldownUntil ? ns(*png.cooldownUntil) : 0) << " cooldown_active=" << png.cooldownActive
                  << " attempted=" << png.preparationAttempted << " attempted_units=" << png.attemptedUnits << " count=" << png.count << " reject_bits=" << png.rejectionReasons
                  << " finished=" << png.finishedAt.has_value() << " finished_ns=" << (png.finishedAt ? ns(*png.finishedAt) : 0)
                  << " finish_cost_known=" << png.finishCost.has_value() << " finish_cost_us=" << (png.finishCost ? png.finishCost->count() : -1)
                  << " success=" << png.finishSuccess << " cache_commit=" << png.cacheCommitted << " cache_abort=" << png.cacheAborted << '\n';
        admissionNumeric(png.admission);
        std::cerr << "comparison_png_clock work=" << s.id << " png_id=" << png.admission.id << " E_ns=" << ns(png.admission.encodeAt)
                  << " prefix_us=" << png.admission.preparationPrefix.count() << " probe_reasons=" << png.admission.probeReasons
                  << " shared_probe_known=" << png.admission.sharedProbeBefore.has_value() << " shared_probe_ns=" << (png.admission.sharedProbeBefore ? ns(*png.admission.sharedProbeBefore) : 0) << '\n';
        for (std::uint32_t i = 0; i < png.count; ++i) {
            const auto& a = png.attempts[i];
            std::cerr << "comparison_png_attempt work=" << s.id << " i=" << i << " width=" << a.width << " height=" << a.height << " encoded=" << a.encoded << " patch=" << a.patch
                      << " bytes_known=" << a.bytes.has_value() << " bytes=" << a.bytes.value_or(0) << " cpu_known=" << a.stages.prepareCpu.has_value() << " cpu_us=" << a.stages.prepareCpu.value_or(0)
                      << " png_known=" << a.stages.pngEncode.has_value() << " png_us=" << a.stages.pngEncode.value_or(0) << '\n';
        }
    }
    for (std::uint32_t i = 0; i < trace.count; ++i) {
        const auto& p = trace.checkpoints[i];
        std::cerr << "readback_trace_point work=" << s.id << " i=" << i << " kind=" << static_cast<unsigned>(p.kind) << " job=" << p.jobId << " revision=" << p.revision
                  << " at_ns=" << ns(p.at) << " at_or_after_D=" << p.atOrAfterDeadline << " remaining_us=" << p.remaining.count()
                  << " epoch=" << p.captureEpoch << " serial=" << p.sourceSerial << " generation=" << p.generation << " base=" << p.composedBase
                  << " cpu_min_us=" << p.cpuMinimum.count() << " overlap_us=" << p.overlappingWork.count() << " observation_units=" << p.observationWorkUnits
                  << " needs_drain=" << p.physicalNeedsDrain << " ready_owned=" << p.readyOwned << " status_known=" << p.status.has_value() << " status=" << p.status.value_or(0)
                  << " error_known=" << p.error.has_value() << " error=" << p.error.value_or(0)
                  << " timer_requested=" << p.timer.requested << " timer_active=" << p.timer.active << " timer_period=" << p.timer.periodMilliseconds
                  << " caps_known=" << p.timer.capabilitiesResult.has_value() << " caps=" << p.timer.capabilitiesResult.value_or(0)
                  << " begin_known=" << p.timer.beginResult.has_value() << " begin=" << p.timer.beginResult.value_or(0) << '\n';
    }
}
asio::awaitable<agent::DesktopVideoUpdate> baseline(agent::DesktopVideoRuntime& video, bool refresh = false) {
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < until) {
        auto update = co_await video.next(refresh);
        if (update && !update->frame.data().empty()) {
            co_return std::move(*update);
        }
    }
    throw std::runtime_error("Runtime fixture did not establish a video baseline");
}
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
asio::awaitable<agent::DesktopVideoUpdate> primaryOutput(agent::DesktopVideoRuntime& video) {
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < until) {
        auto update = co_await video.next();
        if (update && (!update->frame.data().empty() || update->region)) {
            co_return std::move(*update);
        }
    }
    throw std::runtime_error("New Runtime case did not receive a legal primary output");
}
#endif
#ifndef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
asio::awaitable<void> legacy(agent::DesktopVideoRuntime& video) {
    wire::DesktopVideoCapabilities capabilities;
    capabilities.add_codecs(wire::AVC);
    capabilities.set_region_updates(true);
    capabilities.set_tile_cache(true);
    video.capabilities(capabilities);
    unsigned regions = 0, references = 0;
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (regions < 16 && Clock::now() < until) {
        auto update = co_await video.next();
        if (!update) {
            continue;
        }
        require(!update->frame.has_stage_timing() && (!update->region || !update->region->has_stage_timing()), "Legacy emitted V2 stages");
        if (update->region) {
            require(update->region->patches_size() == 1 && update->region->copies_size() == 0, "Small dirty update became whole-frame traffic");
            for (const auto& patch : update->region->patches()) {
                references += patch.png().empty();
            }
            ++regions;
        }
        fixture::displayed(video, *update, false);
    }
    require(regions == 16 && references > 0, "Legacy reliable regions/cache did not reach receiver");
    const auto refreshed = co_await video.next(true);
    require(refreshed && !refreshed->frame.data().empty() && refreshed->frame.key_frame(), "Refresh did not return to a video baseline");
    fixture::displayed(video, *refreshed, false);
}
#endif
asio::awaitable<void> v2(agent::DesktopVideoRuntime& video, bool expectActive) {
    auto first = co_await baseline(video);
    require(first.configuration && first.configuration->desktop_optimization_contract() == 2 && !first.frame.has_stage_timing(), "First Config support accidentally activated V2");
    fixture::displayed(video, first, true);
    wire::DesktopVideoCapabilities capabilities;
    capabilities.add_codecs(wire::AVC);
    capabilities.set_desktop_optimization_contract(2);
    capabilities.set_region_updates(true);
    capabilities.set_tile_cache(true);
    fixture::validator->setOptimizationContract(expectActive ? 2 : 0);
    video.capabilities(capabilities);
    auto configured = co_await baseline(video, true);
    require(!expectActive || (configured.configuration && configured.generation > first.generation && configured.frame.key_frame()), "V2 request had no generation barrier");
    require(configured.frame.has_stage_timing() == expectActive, "Server/browser exact gate failed");
    fixture::displayed(video, configured, true);
    if (!expectActive) {
        co_return;
    }
    const auto& timing = configured.frame.stage_timing();
    require(timing.has_worker_queue_microseconds() && timing.has_acquire_wait_microseconds() && timing.has_source_age_microseconds() && timing.has_pacing_credit_wait_microseconds() && timing.has_video_encode_microseconds(), "Executed stages lost presence");
    require(!timing.has_png_encode_microseconds(), "Unused PNG stage was manufactured as zero");
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    const auto comparisonsBefore = fixture::comparisons.load(), rememberedBefore = fixture::remembered.load();
    ++fixture::version; // Complete CPU startup snapshot is no longer current.
    auto gpuSource = co_await baseline(video);
    fixture::displayed(video, gpuSource, true);
    if (gpuSource.refinement) {
        video.refinementAck(fixture::ack(gpuSource));
    }
    configured = std::move(gpuSource);
    require(fixture::knownCheapStatic && configured.frame.data().size() >= fixture::kKnownVideoBytes &&
                configured.frame.target_frames_per_second() == 30 && configured.frame.target_bitrate() == 24000000,
            "Known-cheap static fixture lost its synthetic VIDEO/QoS premise");
    const auto initialCost = video.diagnostics().optional;
    require(initialCost.roiSource && !initialCost.cpuSource && initialCost.probe && initialCost.unknownPrediction,
            "Known-cheap fixture bypassed cold ROI probe or borrowed CPU history");
    const auto fullBefore = fixture::fullReads.load();
    const auto normalBefore = initialCost.normal;
    unsigned normalBatches = 0;
    std::uint64_t minimumAdvantageMargin = 60000000;
    std::uint32_t minimumAdmissionFps = 120, maximumAdmissionFps = 0;
#endif
    std::set<std::pair<unsigned, unsigned>> seen;
    std::optional<agent::DesktopVideoUpdate> held;
    unsigned batches = 0;
    bool sawPng = configured.refinement && configured.refinement->stage_timing().has_png_encode_microseconds();
    bool sawPngReuse = false;
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (seen.size() <= 32 && Clock::now() < until) {
        auto update = co_await video.next();
        if (!update) {
            continue;
        }
        fixture::displayed(video, *update, true);
        if (!update->refinement) {
            continue;
        }
        require(update->frame.data().empty() && update->refinement->kind() == wire::STATIC_DETAIL && update->frameId == configured.frameId, "Static batch changed video/composed baseline");
        require(update->refinement->batch_id() > batches && update->refinement->has_stage_timing(), "Batch IDs/timing missing");
        require(update->refinementAdmission.has_value(), "Published static batch lost its owned admission association");
        const auto& association = *update->refinementAdmission;
        const auto& admission = association.admission;
        require(association.batch == agent::DesktopDetailBatchKey{update->generation, update->frameId, update->refinement->batch_id()} &&
                    association.work.generation == update->generation && association.work.frame == update->frameId && association.work.slice &&
                    admission.id && admission.jobId && association.completedJobId >= admission.jobId && admission.revision == video.diagnostics().lastJobRevision,
                "Static snapshot belongs to a different job/work/batch");
        require(admission.sourceGeneration == update->generation && admission.composedBase == update->frameId && admission.captureEpoch == fixture::captureEpoch &&
                    admission.sourceSerial == fixture::version && admission.width == 1024 && admission.height == 512 && admission.rotation == 1 &&
                    admission.desktopLeft == fixture::left && admission.desktopTop == fixture::top && !admission.region,
                "Static admission SourceTag did not match the published source");
        require(video.diagnostics().latestPublishedRefinement == update->refinementAdmission, "Owner diagnostics did not retain the accepted batch's exact immutable snapshot");
        const auto& detailStages = update->refinement->stage_timing();
        require(detailStages.has_prepare_cpu_microseconds() && !detailStages.has_video_encode_microseconds(), "Static detail lost executed stage presence or invented video work");
        sawPng |= detailStages.has_png_encode_microseconds();
        sawPngReuse |= !detailStages.has_png_encode_microseconds(); // Local PNG reuse did not run WIC.
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
        require(detailStages.has_gpu_sync_wait_microseconds() && detailStages.has_readback_ready_microseconds(), "Completed ROI lost real Map/readback-ready timing");
        require(admission.source == agent::DesktopPreparationSource::kRoi, "Static snapshot borrowed CPU history for ROI");
        if (admission.choice == agent::DesktopRegionCost::Admission::kNormal) {
            require(admission.framesPerSecond && admission.bitrate && admission.preparePrediction && admission.messageBytes && admission.sendPrediction,
                    "Normal static batch lacks its own actual admission observations");
            const auto allowance = std::chrono::microseconds(std::clamp(500000u / admission.framesPerSecond, 4000u, 12000u));
            require(admission.allowance == allowance && admission.window == allowance && admission.deadline == admission.started + allowance && admission.admittedAt >= admission.started &&
                        admission.remainingAtAdmission == std::chrono::duration_cast<std::chrono::microseconds>(admission.deadline - admission.admittedAt) &&
                        admission.remainingAtAdmission <= allowance && admission.workUnits >= 24576,
                    "Static admission enlarged/slid D or failed to deduct metadata time");
            const auto send = *admission.messageBytes * 8000000 / admission.bitrate;
            require(admission.sendPrediction->count() == static_cast<std::int64_t>(send), "Static send used historical VIDEO bitrate instead of admitted Worker rate");
            require(admission.videoBytes == configured.frame.data().size() && admission.videoBytes >= fixture::kKnownVideoBytes,
                    "Static snapshot VIDEO alternative lost its actual synthetic size");
            const auto videoSend = admission.videoBytes * 8000000 / admission.bitrate;
            require(admission.videoAlternative == admission.videoEncodeEstimate + std::chrono::microseconds(videoSend), "Static VIDEO alternative changed after admission");
            // Preserve >=2ms under stricter ENTER even ignoring VIDEO CPU.
            // Rate comes from this immutable admission, never the old frame or
            // Flow's next adaptive rate after observeFrame.
            const auto enterLimit = videoSend * 4 / 5;
            const auto predicted = admission.preparePrediction->count() + static_cast<std::int64_t>(send);
            if (predicted + 2000 > static_cast<std::int64_t>(enterLimit) || *admission.preparePrediction > admission.remainingAtAdmission ||
                admission.fitDenied || admission.advantageDenied || admission.unknownPrediction) {
                std::cerr << "known_cost_premise batch=" << association.batch.batch << " work=" << association.work.slice << " completed_job=" << association.completedJobId << '\n';
                admissionNumeric(admission);
                require(false, "Static coverage lacks its actual admission compute/advantage premise");
            }
            minimumAdvantageMargin = std::min(minimumAdvantageMargin, enterLimit - static_cast<std::uint64_t>(predicted));
            minimumAdmissionFps = std::min(minimumAdmissionFps, admission.framesPerSecond);
            maximumAdmissionFps = std::max(maximumAdmissionFps, admission.framesPerSecond);
            ++normalBatches;
        }
#endif
        fixture::verifyPixels(*update->refinement, fixture::version);
        for (const auto& patch : update->refinement->patches()) {
            require(seen.emplace(patch.destination().x(), patch.destination().y()).second, "Repeated static tile in a baseline");
        }
        ++batches;
        if (!held) {
            held = *update;
            auto wrong = fixture::ack(*update);
            wrong.mutable_desktop_refinement_ack()->set_batch_id(wrong.desktop_refinement_ack().batch_id() + 1);
            video.refinementAck(wrong);
            for (unsigned attempt = 0; attempt < 4; ++attempt) {
                auto blocked = co_await video.next();
                require(!blocked || !blocked->refinement, "Wrong ACK released refinement credit");
                require(video.diagnostics().latestPublishedRefinement == held->refinementAdmission && held->refinementAdmission == update->refinementAdmission,
                        "Later cache/worker diagnostics overwrote the held published admission snapshot");
            }
        }
        auto correct = fixture::ack(*update);
        video.refinementAck(correct);
        video.refinementAck(correct);
    }
    if (seen.size() <= 32 || batches < 5) {
        const auto diagnostics = video.diagnostics();
        std::cerr << "static_progress tiles=" << seen.size() << " batches=" << batches << " normal=" << diagnostics.optional.normal << " probes=" << diagnostics.optional.probe
                  << " fit=" << diagnostics.optional.fitDenied << " advantage=" << diagnostics.optional.advantageDenied << " unknown=" << diagnostics.optional.unknownPrediction << '\n';
        if (diagnostics.lastAdmission) {
            admissionNumeric(*diagnostics.lastAdmission);
        }
    }
    require(seen.size() > 32 && batches >= 5, "Real runtime never progressed beyond 32 detail candidates");
    require(sawPng && sawPngReuse, "Actual PNG encode/cache-reuse stage presence was not distinguished");
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    require(fixture::submitted > 0 && fixture::fullReads == fullBefore && fixture::comparisons == comparisonsBefore && fixture::remembered == rememberedBefore, "V2 escaped to synchronous/full optional GPU work");
    require(normalBatches >= 5 && video.diagnostics().optional.normal > normalBefore, "Static coverage relied on cold/slow probes instead of normal admission");
    std::cout << "Known-cheap static mechanism fixture tiles=" << seen.size() << " batches=" << batches << " normal_batches=" << normalBatches
              << " video_bytes_floor=" << fixture::kKnownVideoBytes << " min_enter_margin_us=" << minimumAdvantageMargin
              << " admitted_fps_min=" << minimumAdmissionFps << " admitted_fps_max=" << maximumAdmissionFps << " (actual admission rates; not GPU performance)\n";
#endif
    // Motion retires computation, not an unacked decoder lease. A new video
    // remains deliverable; exact old ACK only cleans the old credit.
    std::optional<agent::DesktopVideoUpdate> unacked;
    const auto acquireUntil = Clock::now() + std::chrono::seconds(2);
    while (!unacked && Clock::now() < acquireUntil) {
        auto update = co_await video.next();
        if (update && update->refinement) {
            unacked = std::move(update);
        }
    }
    require(unacked.has_value(), "No batch for preemption test");
    ++fixture::version;
    auto moved = co_await baseline(video);
    require(!moved.refinement, "Retirement released unacked credit before new video");
    fixture::displayed(video, moved, true);
    video.invalidate();
    auto newGeneration = co_await baseline(video);
    require(newGeneration.generation > moved.generation, "Invalidate did not rebuild generation");
    fixture::displayed(video, newGeneration, true);
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        auto update = co_await video.next();
        require(!update || !update->refinement, "New generation bypassed old unacked lease");
    }
    video.refinementAck(fixture::ack(*unacked));

    // Dynamic details must travel WITH their corresponding video, never instead
    // of it; exact PNG pixels are from the same newly captured source.
    unsigned dynamic = 0;
    const auto dynamicUntil = Clock::now() + std::chrono::seconds(5);
    while (!dynamic && Clock::now() < dynamicUntil) {
        ++fixture::version;
        auto update = co_await baseline(video);
        fixture::displayed(video, update, true);
        if (update.refinement) {
            require(update.refinement->kind() == wire::DYNAMIC_DETAIL && !update.frame.data().empty(), "Dynamic refinement omitted base video");
            require(update.refinement->stage_timing().has_video_encode_microseconds() && update.refinement->stage_timing().has_prepare_cpu_microseconds(), "Same-source dynamic detail lost actual video/preparation timing");
            fixture::verifyPixels(*update.refinement, fixture::version);
            video.refinementAck(fixture::ack(update));
            ++dynamic;
        }
    }
    require(dynamic > 0, "Large dynamic damage never attached same-source detail");
    std::optional<agent::DesktopVideoUpdate> timeoutBatch;
    const auto timeoutAcquire = Clock::now() + std::chrono::seconds(3);
    while (!timeoutBatch && Clock::now() < timeoutAcquire) {
        auto update = co_await video.next();
        if (update && update->refinement) {
            fixture::displayed(video, *update, true);
            timeoutBatch = std::move(update);
        }
    }
    require(timeoutBatch.has_value(), "No batch for ACK timeout test");
    asio::steady_timer timeout(co_await asio::this_coro::executor);
    timeout.expires_after(std::chrono::milliseconds(2100));
    co_await timeout.async_wait(asio::use_awaitable);
    video.refinementAck(fixture::ack(*timeoutBatch)); // Cleanup does not resume this generation.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        auto update = co_await video.next();
        require(!update || !update->refinement, "Timeout ACK resumed paused generation");
    }
    ++fixture::version;
    auto afterTimeout = co_await baseline(video);
    fixture::displayed(video, afterTimeout, true);
    require(!afterTimeout.refinement && !afterTimeout.frame.data().empty(), "Refinement timeout closed or blocked video");
    ++fixture::left;
    ++fixture::version;
    auto relocated = co_await baseline(video);
    fixture::displayed(video, relocated, true);
    require(relocated.configuration && relocated.generation > afterTimeout.generation && relocated.frameId == 1 && relocated.frame.key_frame() && relocated.desktopLeft == fixture::left, "Source geometry/origin changed without generation barrier");
    // A contract downgrade is another generation barrier, not a mid-generation
    // disappearance of fields. Unknown versions must remain legacy.
    capabilities.set_desktop_optimization_contract(3);
    fixture::validator->setOptimizationContract(0);
    video.capabilities(capabilities);
    const auto downgraded = co_await baseline(video);
    require(downgraded.configuration && downgraded.generation > newGeneration.generation && !downgraded.frame.has_stage_timing(), "Unknown capability did not rebuild a legacy generation");
    fixture::displayed(video, downgraded, false);
}
asio::awaitable<void> tinyKnownCost(agent::DesktopVideoRuntime& video) {
    fixture::tinyCpuCost = true;
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    // This case alone establishes a known-cost 30fps premise: real software
    // AVC wall time (no Sleep(8)) and an explicit 12KiB serialized alternative.
    // WIC PNG and independent slow/failure boundaries remain real/unmodified.
    fixture::knownTinyCodec = true;
#endif
    fixture::details = false;
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    caps.set_region_updates(true);
    caps.set_tile_cache(true);
    fixture::validator->setOptimizationContract(2);
    video.capabilities(caps);
    auto first = co_await baseline(video);
    std::cerr << "tiny first encoder_us=" << first.frame.encode_microseconds() << " fps=" << first.frame.target_frames_per_second() << '\n';
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    require(first.frame.encode_microseconds() <= 800000 / 30 && first.frame.data().size() >= fixture::kTinyKnownVideoBytes, "Known tiny codec did not establish real 30fps CPU/message premise");
#endif
    fixture::displayed(video, first, false);
    // Commit the real feedback revision, without refill/probe sleeps.
    first = co_await baseline(video, true);
    std::cerr << "tiny refresh encoder_us=" << first.frame.encode_microseconds() << " fps=" << first.frame.target_frames_per_second() << '\n';
    fixture::displayed(video, first, false);
    fixture::smallDamage = true;
    // Wait for the existing controller to reach 30fps with normal tiny
    // traffic, not a synthetic rate override. The GPU-style codec boundary is
    // explicitly known-cost, not an MFT/hardware performance model. Each
    // source remains held until PRIMARY output (cursor-only is not applied).
    const auto warmUntil = Clock::now() + std::chrono::seconds(10);
    bool ready = false;
    unsigned warmVideos = 0, warmRegions = 0, warmDefinitions = 0, warmReferences = 0;
    std::uint32_t actualFps = first.frame.target_frames_per_second(), actualBitrate = first.frame.target_bitrate();
    const auto warmBefore = video.diagnostics().optional;
    ++fixture::version;
    while (!ready && Clock::now() < warmUntil) {
        auto warm = co_await video.next();
        if (!warm || (warm->frame.data().empty() && !warm->region)) {
            continue;
        }
        fixture::displayed(video, *warm, false);
        const auto fps = warm->region ? warm->region->target_frames_per_second() : warm->frame.target_frames_per_second();
        const auto bitrate = warm->region ? warm->region->target_bitrate() : warm->frame.target_bitrate();
        actualFps = fps;
        actualBitrate = bitrate;
        warmVideos += !warm->frame.data().empty();
        warmRegions += warm->region.has_value();
        if (warm->region) {
            for (const auto& patch : warm->region->patches()) {
                warmDefinitions += !patch.png().empty();
                warmReferences += patch.png().empty();
            }
        }
        ready = fps == 30 && bitrate == 8000000;
        if (!ready) {
            ++fixture::version;
        }
    }
    const auto warmAfter = video.diagnostics().optional;
    std::cerr << "tiny_warm_numeric feedbackPreference=" << wire::QualityFeedback::SMOOTH << " actualFps=" << actualFps << " actualBitrate=" << actualBitrate
              << " videos=" << warmVideos << " regions=" << warmRegions << " definitions=" << warmDefinitions << " references=" << warmReferences
              << " normal=" << warmAfter.normal - warmBefore.normal << " probe=" << warmAfter.probe - warmBefore.probe
              << " fitDenied=" << warmAfter.fitDenied - warmBefore.fitDenied << " advantageDenied=" << warmAfter.advantageDenied - warmBefore.advantageDenied
              << " unknown=" << warmAfter.unknownPrediction - warmBefore.unknownPrediction << " actualPng=" << warmAfter.actualPngBytes - warmBefore.actualPngBytes
              << " prep=" << warmAfter.predictedPrepareMicroseconds.value_or(UINT64_MAX) << " message=" << warmAfter.predictedMessageBytes.value_or(UINT64_MAX)
              << " codecCount=" << fixture::tinyCodecCount << " codecDrop=" << fixture::tinyCodecDropped
              << " knownVideoFloor=" << (fixture::knownTinyCodec ? fixture::kTinyKnownVideoBytes : 0) << '\n';
    for (std::size_t index = 0; index < fixture::tinyCodecCount; ++index) {
        const auto& sample = fixture::tinyCodecSamples[index];
        std::cerr << "tiny_codec_numeric call=" << sample.call << " sleepUs=" << sample.sleepUs << " codecUs=" << sample.codecUs << " wallUs=" << sample.wallUs
                  << " bytes=" << sample.bytes << " fps=" << sample.fps << " bitrate=" << sample.bitrate << " refresh=" << sample.refresh << '\n';
    }
    require(ready, "Existing rate controller did not establish SMOOTH 8Mbps/30fps");
    fixture::tinyFreshPalette = true; // Two new cheap WIC colors plus known reliable slots.
    unsigned regions = 0, definitions = 0, references = 0;
    std::vector<Clock::time_point> regionTimes;
    const auto before = video.diagnostics().optional;
    const auto until = Clock::now() + std::chrono::seconds(10);
    while (regions < 20 && Clock::now() < until) {
        ++fixture::version;
        std::optional<agent::DesktopVideoUpdate> update;
        while (!update && Clock::now() < until) {
            auto candidate = co_await video.next();
            if (candidate && (!candidate->frame.data().empty() || candidate->region)) {
                update = std::move(candidate);
            }
        }
        if (!update) {
            const auto d = video.diagnostics();
            std::cerr << "tiny no output capture=" << d.captureCalls << " encoded=" << d.encodedFrames << " pixelUpdates=" << d.pixelUpdates << " fullReads=" << fixture::fullReads
                      << " normal=" << d.optional.normal << " probe=" << d.optional.probe << " fit=" << d.optional.fitDenied << " adv=" << d.optional.advantageDenied << '\n';
            if (d.lastAdmission) {
                admissionNumeric(*d.lastAdmission);
            }
        }
        require(update.has_value(), "Tiny known CPU source did not produce output");
        fixture::displayed(video, *update, false);
        if (!update->region) {
            continue;
        }
        if (update->region->target_bitrate() != 8000000 || update->region->target_frames_per_second() != 60) {
            if (const auto snapshot = video.diagnostics().lastAdmission) {
                admissionNumeric(*snapshot);
            }
            std::cerr << "tiny rate bitrate=" << update->region->target_bitrate() << " fps=" << update->region->target_frames_per_second() << '\n';
        }
        require(update->region->target_bitrate() == 8000000 && update->region->target_frames_per_second() == 30, "Tiny fixture changed SMOOTH 8Mbps/30fps conditions");
        require(update->region->ByteSizeLong() + 512 <= 8333, "Tiny region exceeded its independent byte budget");
        require(update->region->patches_size() == 1, "Tiny CPU update did not remain one patch");
        const auto& patch = update->region->patches(0);
        require(patch.cache_slot() != 0, "Tiny CPU patch lost reliable tuple/slot");
        if (patch.png().empty()) {
            ++references;
        } else {
            ++definitions;
            wire::VideoRefinement decoded;
            *decoded.add_patches() = patch;
            decoded.mutable_patches(0)->set_cache_slot(0);
            fixture::verifyPixels(decoded, fixture::version);
        }
        ++regions;
        regionTimes.push_back(Clock::now());
    }
    const auto after = video.diagnostics().optional;
    const auto normal = after.normal - before.normal;
    std::cerr << "tiny diagnostic regions=" << regions << " definitions=" << definitions << " references=" << references << " normal=" << normal
              << " probe=" << after.probe - before.probe << " fitDenied=" << after.fitDenied - before.fitDenied
              << " advantageDenied=" << after.advantageDenied - before.advantageDenied << " cpu=" << after.cpuSource - before.cpuSource << " roi=" << after.roiSource - before.roiSource
              << " prep_us=" << after.predictedPrepareMicroseconds.value_or(0) << " predicted_bytes=" << after.predictedMessageBytes.value_or(0) << '\n';
    if (regions != 20 || definitions < 2 || references < 10 || normal < 18 || (regionTimes.size() > 1 && regionTimes.back() - regionTimes.front() >= std::chrono::seconds(2))) {
        if (const auto snapshot = video.diagnostics().lastAdmission) {
            admissionNumeric(*snapshot);
        }
    }
    require(regions == 20 && definitions >= 2 && references >= 10, "Tiny normal WIC/cache region route stayed probe-bound");
    require(normal >= 18 && regionTimes.back() - regionTimes.front() < std::chrono::seconds(2), "Tiny known CPU work failed continuous >1Hz normal admission");
    require(after.predictedMessageBytes && *after.predictedMessageBytes <= 1100 && after.roiSource == before.roiSource && fixture::submitted == 0, "Tiny route used quota/unknown ROI prediction");
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    require(after.cpuSource > before.cpuSource && after.materializeXorCursor == before.materializeXorCursor, "Known CPU source was replaced by a cursor readback");
#endif
    std::cout << "tiny SMOOTH8M/60: regions=" << regions << " normal=" << normal << " definitions=" << definitions << " references=" << references
              << " span_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(regionTimes.back() - regionTimes.front()).count()
              << " predicted_message_bytes=" << *after.predictedMessageBytes << " actual_png_bytes=" << after.actualPngBytes - before.actualPngBytes << '\n';
}
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
asio::awaitable<void> costFailure(agent::DesktopVideoRuntime& video, unsigned mode) {
    co_await tinyKnownCost(video); // Real cheap WIC/cache history, 8Mbps/30fps established.
    const auto before = video.diagnostics();
    const auto calls = fixture::pngCalls.load();
    fixture::costTwoTiles = true;
    fixture::costEntropy = mode == 1;
    fixture::pngFault = mode == 1 ? 0 : mode;
    ++fixture::version;
    auto output = co_await baseline(video);
    fixture::displayed(video, output, false);
    const auto after = video.diagnostics();
    require(!output.region && !output.frame.data().empty(), "Failed optional work suppressed required VIDEO or published a partial region");
    require(fixture::pngCalls == calls + 1, "Optional failure continued to another uncancellable tile");
    require(after.regionBudgetFailures > before.regionBudgetFailures, "Failed optional attempt was not observed");
    if (mode == 3) {
        require(after.optional.unknownPngProducts == before.optional.unknownPngProducts + 1 && !after.optional.lastPngBytes,
                "Missing codec product became zero or fabricated byte quota");
    } else {
        require(after.optional.lastPngBytes && *after.optional.lastPngBytes > (mode == 1 ? 20000u : 0u), "Rejected real WIC PNG size was erased");
        require(after.optional.actualPngBytes > before.optional.actualPngBytes, "Rejected actual bytes were not observed");
    }
    // No 1100ms wait, reset, or refund. The very next source must respect the
    // existing failure cooldown and may not invoke WIC a second time.
    fixture::costEntropy = false;
    fixture::pngFault = 0;
    ++fixture::version;
    auto cooldown = co_await baseline(video);
    fixture::displayed(video, cooldown, false);
    const auto denied = video.diagnostics();
    require(fixture::pngCalls == calls + 1 && !cooldown.region, "PNG failure was washed out by another source/bucket");
    require(denied.optionalCostDenials > after.optionalCostDenials, "Failure cooldown did not reject preparation");
    if (mode >= 2) {
        require(denied.optional.predictedPrepareMicroseconds && *denied.optional.predictedPrepareMicroseconds >= 25000 && denied.optional.fitDenied > after.optional.fitDenied,
                "Slow/failed PNG sunk CPU was diluted by the unattempted second tile");
    }
    std::cout << "cost failure mode=" << mode << " png_calls=1 actual_png_bytes=";
    if (after.optional.lastPngBytes) {
        std::cout << *after.optional.lastPngBytes;
    } else {
        std::cout << "null";
    }
    std::cout << " cooldown=retained subsequent_tile=stopped\n";
}
#endif
asio::awaitable<void> reliableV2(agent::DesktopVideoRuntime& video) {
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    caps.set_region_updates(true);
    caps.set_tile_cache(true);
    fixture::validator->setOptimizationContract(2);
    video.capabilities(caps);
    auto first = co_await baseline(video);
    fixture::displayed(video, first, true);
    fixture::smallDamage = true;
    unsigned regions = 0;
    const auto until = Clock::now() + std::chrono::seconds(10);
    while (regions < 3 && Clock::now() < until) {
        ++fixture::version;
        const auto attemptUntil = Clock::now() + std::chrono::milliseconds(300);
        while (Clock::now() < attemptUntil) {
            auto update = co_await video.next();
            if (!update) {
                continue;
            }
            fixture::displayed(video, *update, true);
            if (update->refinement) {
                video.refinementAck(fixture::ack(*update));
            }
            if (update->region) {
                require(update->frame.data().empty() && update->region->patches_size() == 1 && update->region->has_stage_timing(), "Small V2 damage did not remain a complete reliable region");
                require(update->region->patches(0).cache_slot() != 0, "Reliable cache path disappeared in V2");
                require(update->region->stage_timing().has_prepare_cpu_microseconds() && !update->region->stage_timing().has_video_encode_microseconds(), "Reliable region reported wrong executed stages");
                if (!update->region->patches(0).png().empty()) {
                    wire::VideoRefinement decoded;
                    *decoded.add_patches() = update->region->patches(0);
                    decoded.mutable_patches(0)->set_cache_slot(0);
                    fixture::verifyPixels(decoded, fixture::version);
                }
                ++regions;
                break;
            }
            if (!update->frame.data().empty()) {
                // Optional WIC work can correctly miss its absolute deadline.
                // Give the production 1Hz failure probe a real scheduled retry,
                // rather than hammer fresh sources throughout its cooldown.
                asio::steady_timer retry(co_await asio::this_coro::executor);
                retry.expires_after(std::chrono::milliseconds(1100));
                co_await retry.async_wait(asio::use_awaitable);
                break;
            }
        }
    }
    if (regions != 3) {
        const auto diagnostics = video.diagnostics();
        std::cerr << "reliable regions=" << regions << " submitted=" << fixture::submitted << " pending=" << diagnostics.roiPending << " retired=" << diagnostics.roiRetired << " budgetFailures=" << diagnostics.regionBudgetFailures << " denials=" << diagnostics.optionalCostDenials << '\n';
    }
    require(regions == 3, "Reliable V2 ROI was never selected");
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    require(fixture::submitted > 0 && fixture::fullReads == 0 && fixture::comparisons == 0 && fixture::remembered == 0, "Reliable ROI used optional synchronous/full GPU work");
#endif
}
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
asio::awaitable<void> readbackOrdering(agent::DesktopVideoRuntime& video, unsigned mode) {
    fixture::boundedOrdering = true;
    fixture::details = mode != 1; // Flat one-tile damage isolates ordering from codec pressure.
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    caps.set_region_updates(mode == 1);
    fixture::validator->setOptimizationContract(2);
    video.capabilities(caps);
    auto first = co_await baseline(video);
    fixture::displayed(video, first, true);
    // Commit the real SHARP/feedback revision barrier before testing optional
    // work; its required keyframe must not be mistaken for a readback fallback.
    first = co_await baseline(video, true);
    fixture::displayed(video, first, true);
    {
        // Establish an executable preparation opportunity after the baseline's
        // earliest encode time; refill tokens without a Runtime call/static slice.
        // This happens before new damage and does not extend its admission D.
        asio::steady_timer tokens(co_await asio::this_coro::executor);
        tokens.expires_after(std::chrono::milliseconds(40));
        co_await tokens.async_wait(asio::use_awaitable);
    }
    fixture::smallDamage = mode == 1;
    fixture::pendingChecks = mode == 1 ? 1 : 0;
    fixture::readyDuringEncode = mode != 1;
    fixture::resetDuringEncode = mode == 3;
    fixture::stalled = mode == 4;
    ++fixture::version;
    const auto wantedSource = fixture::version.load();
    bool published = false, observedPending = false;
    unsigned acquiredAtSubmit = 0, controlEvents = 0;
    std::optional<agent::DesktopReadbackDiagnostic> pendingWork;
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (!published && Clock::now() < until) {
        const auto checks = fixture::checked.load();
        auto update = co_await video.next();
        require(fixture::checked - checks <= 1, "More than one nonblocking check in a Runtime scheduling step");
        if (mode == 1 && fixture::submitted && !observedPending) {
            require(video.diagnostics().roiPending == 1 && (!update || !update->region), "Region did not start as immediate Pending");
            acquiredAtSubmit = fixture::acquisitions;
            // Every subsequent Acquire WOULD replace sourceSerial. Ready must
            // reach the original decision point first, not a frozen source test.
            fixture::advanceOnAcquire = true;
            observedPending = true;
            pendingWork = video.diagnostics().lastReadback;
            require(pendingWork && pendingWork->purpose == 1, "Ordering lacked ordinary work association");
            require(pendingWork->admission.started >= pendingWork->admission.encodeAt,
                    "Ordering lacked an immediately executable preparation opportunity");
            // Exactly one ordinary public-control event. post executes only after
            // the next next() has installed its pace wait, not inline beforehand.
            asio::post(co_await asio::this_coro::executor, [&video, caps, &controlEvents, &pendingWork] {
                const auto before = video.diagnostics();
                video.capabilities(caps);
                ++controlEvents;
                const auto after = video.diagnostics();
                require(after.ownerRevision == before.ownerRevision && after.lastReadback && after.lastReadback->admission == pendingWork->admission,
                        "Same-caps control changed revision or pending admission");
            });
        }
        if (!update) {
            continue;
        }
        fixture::displayed(video, *update, true);
        if (mode == 1) {
            if (!update->frame.data().empty()) {
                const auto diagnostics = video.diagnostics();
                std::cerr << "ordering region: submitted=" << fixture::submitted << " checked=" << fixture::checked << " pending=" << diagnostics.roiPending
                          << " retired=" << diagnostics.roiRetired << " budgetFailures=" << diagnostics.regionBudgetFailures << " denials=" << diagnostics.optionalCostDenials
                          << " acquisitions=" << fixture::acquisitions << " source=" << fixture::version << " configuration=" << update->configuration.has_value() << '\n';
                readbackDiagnostic(diagnostics);
            }
            require(update->frame.data().empty(), "Pending->Ready region lost its original encoding window");
            if (update->region) {
                require(observedPending && fixture::acquisitions == acquiredAtSubmit && fixture::version == wantedSource && fixture::submittedSource == wantedSource,
                        "Ready was overwritten by next Acquire/new serial");
                wire::VideoRefinement decoded;
                *decoded.add_patches() = update->region->patches(0);
                decoded.mutable_patches(0)->set_cache_slot(0);
                fixture::verifyPixels(decoded, wantedSource);
                const auto d = video.diagnostics();
                require(controlEvents == 1 && pendingWork && d.lastReadback && d.lastReadback->admission == pendingWork->admission && d.lastJobConsumedReady && !d.lastJobAcquired,
                        "Ordering lost original T/E/D/rate/source/revision or consumed after Acquire");
                unsigned starts = 0, ready = 0;
                for (unsigned index = 0; index < d.lastReadback->count; ++index) {
                    const auto& point = d.lastReadback->checkpoints[index];
                    if (point.kind == agent::DesktopReadbackCheckpointKind::kCheckStart) {
                        ++starts;
                        require(!point.atOrAfterDeadline && point.sourceSerial == wantedSource && point.revision == pendingWork->admission.revision,
                                "Ordering second check was late or belonged to another source/revision");
                    }
                    ready += point.kind == agent::DesktopReadbackCheckpointKind::kReady;
                }
                require(starts == 2 && ready == 1 && d.lastReadback->retirementReason == agent::DesktopVideoFallbackReason::kNone,
                        "Ordering did not observe genuine Ready in its original window");
                published = true;
            }
        } else if (!update->frame.data().empty()) {
            require(fixture::submitted == 1 && fixture::submittedSource == wantedSource && fixture::encodedSource == wantedSource,
                    "Dynamic test did not overlap one real encode with submitted ROI");
            if (mode == 3) {
                require(update->configuration && update->generation > first.generation && !update->refinement && fixture::checked == 0,
                        "Encoder baseline reset consumed old generation ROI");
            } else if (mode == 4) {
                require(!update->refinement && fixture::checked == 1 && video.diagnostics().roiPending == 1 && video.diagnostics().roiRetired == 1,
                        "Still-Pending dynamic readback blocked video, polled twice, or survived its decision deadline");
            } else {
                require(update->refinement && update->refinement->kind() == wire::DYNAMIC_DETAIL && fixture::checked == 1 && video.diagnostics().roiPending == 0,
                        "Dynamic completion was not checked exactly once AFTER encoding");
                fixture::verifyPixels(*update->refinement, wantedSource);
            }
            published = true;
        }
    }
    require(published && fixture::fullReads == 0 && fixture::comparisons == 0, "Readback ordering test never published or escaped to synchronous pixels");
    std::cout << "Readback ordering mode=" << mode << " submitted=" << fixture::submitted << " checked=" << fixture::checked << " passed\n";
    readbackDiagnostic(video.diagnostics());
}
asio::awaitable<void> pendingFallback(agent::DesktopVideoRuntime& video) {
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    caps.set_region_updates(true);
    caps.set_tile_cache(true);
    fixture::validator->setOptimizationContract(2);
    video.capabilities(caps);
    auto first = co_await baseline(video);
    fixture::displayed(video, first, true);
    fixture::smallDamage = true;
    fixture::stalled = true;
    ++fixture::version;
    auto pending = co_await baseline(video);
    fixture::displayed(video, pending, true);
    require(!pending.region && !pending.refinement && pending.frame.has_stage_timing(), "Pending beyond anchored D blocked/replaced video");
    require(video.diagnostics().regionDecisionWaits > 0 && video.diagnostics().regionDecisionExpirations > 0, "Pending did not use its fixed decision window before VIDEO fallback");
    require(fixture::submitted > 0 && video.diagnostics().roiPending > 0 && fixture::fullReads == 0 && fixture::comparisons == 0, "Pending used synchronous optional GPU fallback");
    fixture::smallDamage = false;
    ++fixture::version;
    auto busy = co_await baseline(video);
    fixture::displayed(video, busy, true);
    require(!busy.refinement && fixture::submitted == 1, "Retired physical slot was prematurely reused");
    fixture::stalled = false;
    fixture::unavailable = true;
    ++fixture::version;
    auto unavailable = co_await baseline(video);
    fixture::displayed(video, unavailable, true);
    require(!unavailable.region && !unavailable.refinement, "Unavailable optional ROI blocked video");
    fixture::unavailable = false;
    fixture::stale = true;
    asio::steady_timer cooldown(co_await asio::this_coro::executor);
    cooldown.expires_after(std::chrono::milliseconds(1100));
    co_await cooldown.async_wait(asio::use_awaitable);
    ++fixture::version;
    auto old = co_await baseline(video);
    fixture::displayed(video, old, true);
    require(fixture::submitted > 1 && !old.refinement && !old.region, "Drained slot never resumed or stale ROI mutated current damage/baseline/cache");
    require(fixture::checked <= video.diagnostics().captureCalls + 1, "Runtime busy-polled ROI completion");
}
#endif
asio::awaitable<void> permissionGate(agent::DesktopVideoRuntime& video, unsigned server) {
    // Match real Worker startup: immutable Server support, initial joint allow0,
    // then Browser caps and the explicit pending-permission setter, in that order.
    agent::DesktopOptimizationGate workerGate(server);
    video.setOptimizationContract(workerGate.allowedContract());
    auto first = co_await baseline(video);
    require(first.configuration && first.configuration->desktop_optimization_contract() == 2 && !first.frame.has_stage_timing(), "Initial allow0 erased Config2 support or activated V2");
    workerGate.configure(*first.configuration);
    require(workerGate.activeContract() == 0, "Worker activated first support-only Config2");
    fixture::displayed(video, first, false);
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    video.capabilities(caps);
    auto blocked = co_await baseline(video, true);
    require(!blocked.frame.has_stage_timing(), "Browser caps bypassed joint allow0");
    fixture::displayed(video, blocked, false);
    workerGate.request(caps.desktop_optimization_contract());
    fixture::validator->setOptimizationContract(workerGate.allowedContract());
    caps.set_desktop_optimization_contract(workerGate.allowedContract());
    video.setOptimizationContract(workerGate.allowedContract());
    video.capabilities(caps);
    auto enabled = co_await baseline(video, server != 2);
    if (enabled.configuration) {
        workerGate.configure(*enabled.configuration);
    }
    require(enabled.frame.has_stage_timing() == (server == 2) && workerGate.activeContract() == (server == 2 ? 2u : 0u), "Setter changed immutable Server support");
    require(server != 2 || (enabled.configuration && enabled.generation > blocked.generation && enabled.frameId == 1), "Setter0->2 lacked new Config/generation");
    fixture::displayed(video, enabled, false);
    if (server != 2) {
        video.setOptimizationContract(2);
        caps.set_desktop_optimization_contract(2);
        video.capabilities(caps);
        auto forbidden = co_await baseline(video, true);
        require(!forbidden.frame.has_stage_timing(), "Joint setter overwrote immutable unsupported Server");
        co_return;
    }

    // Permission changes while a real production Runtime worker job is pending.
    // Its result must be discarded completely, not just stripped of refinement.
    fixture::holdCapture = true;
    asio::steady_timer owner(co_await asio::this_coro::executor);
    bool changed = false;
    auto change = [&]() -> asio::awaitable<void> {
        while (!fixture::captureHeld) {
            owner.expires_after(std::chrono::milliseconds(1));
            co_await owner.async_wait(asio::use_awaitable);
        }
        workerGate.request(0);
        video.setOptimizationContract(workerGate.allowedContract());
        changed = true;
        fixture::releaseCapture = true;
    };
    std::exception_ptr error;
    asio::co_spawn(owner.get_executor(), change(), [&](std::exception_ptr failure) { error = failure; });
    auto staleJob = co_await video.next(true);
    require(!error && changed && !staleJob, "Permission switch published old inflight worker output");
    fixture::validator->setOptimizationContract(0);
    auto downgraded = co_await baseline(video);
    require(downgraded.configuration && downgraded.generation > enabled.generation && downgraded.frameId == 1 && !downgraded.frame.has_stage_timing(), "Inflight permission downgrade did not rebuild legacy baseline");
    fixture::displayed(video, downgraded, false);
    video.setOptimizationContract(3);
    auto unknown = co_await baseline(video, true);
    require(!unknown.frame.has_stage_timing(), "Unknown joint contract activated V2");
}
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
asio::awaitable<void> roiRecovery(agent::DesktopVideoRuntime& video, unsigned fault) {
    wire::DesktopVideoCapabilities caps;
    caps.add_codecs(wire::AVC);
    caps.set_desktop_optimization_contract(2);
    caps.set_region_updates(true);
    video.setOptimizationContract(0);
    auto first = co_await baseline(video);
    fixture::displayed(video, first, true);
    fixture::validator->setOptimizationContract(2);
    video.setOptimizationContract(2);
    video.capabilities(caps);
    auto active = co_await baseline(video);
    fixture::displayed(video, active, true);
    if (fault <= 4) {
        fixture::smallDamage = true;
        ++fixture::version;
        if (fault < 3) {
            fixture::submitReset = static_cast<int>(fault);
        } else {
            fixture::completeReset = static_cast<int>(fault - 2);
        }
    } else {
        // A Pending static ROI completes on the next round, BEFORE Acquire.
        // This covers both completion sites, not just submit's immediate check.
        ++fixture::version;
        auto source = co_await baseline(video);
        fixture::displayed(video, source, true);
        if (source.refinement) {
            video.refinementAck(fixture::ack(source));
        }
        fixture::stalled = true;
        const auto submissions = fixture::submitted.load();
        const auto pendingUntil = Clock::now() + std::chrono::seconds(3);
        while (fixture::submitted == submissions && Clock::now() < pendingUntil) {
            co_await video.next();
        }
        require(fixture::submitted > submissions && video.diagnostics().roiPending, "No pending static ROI for pre-Acquire reset");
        fixture::completeReset = static_cast<int>(fault - 4);
        fixture::stalled = false;
    }
    const auto until = Clock::now() + std::chrono::seconds(5);
    std::optional<agent::DesktopVideoUpdate> recovered;
    while (!recovered && Clock::now() < until) {
        auto update = co_await video.next();
        if (!update) {
            continue;
        }
        require(!update->region && !update->refinement, "Lost-source ROI submitted old region/detail");
        if (!update->frame.data().empty()) {
            recovered = std::move(update);
        }
    }
    require(fixture::resets == 1 && recovered && recovered->configuration && recovered->generation > active.generation && recovered->frameId == 1 && recovered->frame.key_frame() && recovered->desktopLeft == fixture::left, "ROI reset did not recover a fresh source/generation");
    fixture::displayed(video, *recovered, !fixture::delayedReset);
    if (fixture::delayedReset) {
        // No extra slow-probe slot/refill: inspect the very next source's ROI
        // prediction after a real delayed submit/completion exception.
        ++fixture::version;
        auto follow = co_await baseline(video);
        fixture::displayed(video, follow, false);
        const auto predicted = video.diagnostics().optional.predictedPrepareMicroseconds;
        require(predicted && *predicted >= 15000, "Platform exception CPU was erased by pacing overlap");
        std::cout << "delayed ROI exception fault=" << fault << " predicted_prepare_us=" << *predicted << '\n';
    }
    require(fixture::fullReads == 0 && fixture::comparisons == 0, "ROI reset escaped to synchronous optional readback");
    // Failure budget is bounded even if every next capture/ROI loses its device.
    bool exhausted = false;
    const auto failUntil = Clock::now() + std::chrono::seconds(8);
    while (!exhausted && Clock::now() < failUntil) {
        fixture::submitReset = 1;
        ++fixture::version;
        try {
            auto update = co_await video.next();
            if (update) {
                fixture::displayed(video, *update, true);
            }
        } catch (const agent::DesktopFailure& error) {
            require(error.code() == wire::DesktopFault::CAPTURE_DEVICE_LOST && fixture::resets == 4 && std::string_view(error.what()) == "Desktop capture recovery budget exhausted", "Unexpected failure masqueraded as bounded ROI recovery");
            exhausted = true;
        }
    }
    require(exhausted, "Repeated optional ROI failures escaped bounded recovery");
}
#endif
void run(unsigned gate, bool legacyMode, bool pendingMode = false, bool reliableMode = false, bool permissionMode = false, unsigned fault = 0, unsigned orderingMode = 0, bool tinyMode = false, unsigned costMode = 0, bool delayedReset = false) {
    fixture::reset(!legacyMode);
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    fixture::knownCheapStatic = gate == 2 && !legacyMode && !pendingMode && !reliableMode && !permissionMode && !fault && !orderingMode && !tinyMode && !costMode;
    // Declared before the runtime: worker/encoder teardown happens before this
    // mode is restored, including exception exits. It cannot leak to a case.
    struct RestoreKnownCost {
        ~RestoreKnownCost() { fixture::knownCheapStatic = false; }
    } restoreKnownCost;
#endif
    fixture::delayedReset = delayedReset;
    fixture::sequence = 1;
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, gate);
    std::exception_ptr failure;
    asio::co_spawn(attachment.executor(),
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
                   costMode ? costFailure(video, costMode) :
#endif
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
                   tinyMode         ? tinyKnownCost(video)
                   : orderingMode   ? readbackOrdering(video, orderingMode)
                   : fault          ? roiRecovery(video, fault)
                   : permissionMode ? permissionGate(video, gate)
                   : reliableMode   ? reliableV2(video)
                   : pendingMode    ? pendingFallback(video)
                                    : v2(video, true),
#else
                   tinyMode ? tinyKnownCost(video) : permissionMode ? permissionGate(video, gate)
                                                 : reliableMode     ? reliableV2(video)
                                                 : legacyMode       ? legacy(video)
                                                                    : v2(video, gate == 2),
#endif
                   [&](std::exception_ptr error) { failure = error; video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        std::cerr << "runtime_case_failure gate=" << gate << " ordering=" << orderingMode << " fault=" << fault << " tiny=" << tinyMode << " cost=" << costMode << '\n';
        readbackDiagnostic(video.diagnostics());
        std::rethrow_exception(failure);
    }
#ifndef ULTRATERIMAL_PNG_COST_FIXTURE
    static_cast<void>(costMode);
#endif
#ifndef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
    static_cast<void>(pendingMode);
    static_cast<void>(fault);
    static_cast<void>(orderingMode);
#endif
}
} // namespace
void runtimePrefixPolicyReplay(const agent::DesktopPrimaryAdmission& association) {
    using namespace std::chrono;
    using Cost = agent::DesktopRegionCost;
    const auto held = association;
    const auto& snapshot = association.admission;
    require(snapshot.region && !snapshot.comparison && snapshot.source == agent::DesktopPreparationSource::kCpu && snapshot.sourceGeneration == association.generation && snapshot.composedBase == association.base && snapshot.preparationPrefix > microseconds::zero(), "Prefix replay was not bound to a genuine Runtime primary source/batch");
    // Deterministic policy replay complements, never substitutes for, the
    // actual Runtime batch gate above. No production clock/rate override.
    agent::DesktopOptionalProbe probe;
    Cost cost(&probe);
    const auto origin = Cost::Clock::time_point{} + seconds(100);
    cost.begin(origin, microseconds(8333), snapshot.source, snapshot.workUnits);
    cost.observe(microseconds(500), true, origin + microseconds(500));
    probe.last = origin + microseconds(500);
    const auto now = origin + milliseconds(1);
    cost.begin(now, microseconds(600), snapshot.source, snapshot.workUnits);
    require(!cost.admit(now, microseconds(1100), microseconds(100), microseconds(400)) && !cost.fitDenied() && cost.advantageDenied() && cost.preparePrediction() == microseconds(500), "Sunk prefix was deducted twice from FIT, omitted from advantage, or written into PNG history");
    cost.begin(now, microseconds(600), snapshot.source, snapshot.workUnits);
    require(cost.admit(now, microseconds(1100), microseconds(100)) && cost.admission() == Cost::Admission::kNormal, "Prefix=0 negative control did not enter normal work against the SAME original V");
    cost.begin(now, microseconds(600), snapshot.source, snapshot.workUnits);
    require(!cost.admit(now, microseconds(700), microseconds(100), microseconds(400)) && !cost.fitDenied() && cost.advantageDenied(), "Retain advantage ignored spent comparison prefix");
    require(association == held && snapshot.framesPerSecond == held.admission.framesPerSecond && snapshot.videoAlternative == held.admission.videoAlternative, "Prefix replay overwrote the primary snapshot or borrowed mutable VIDEO rate");
    std::cout << "Runtime source-key primary association + deterministic prefix replay: remaining600/PNG500/send100/prefix400/originalV1100 ENTER denied by advantage not FIT; prefix0 normal; retain V700 denied\n";
}
void regionDecisionClockCases() {
    using namespace std::chrono;
    using Decision = ultraterimal::agent::DesktopRegionDecision;
    const auto origin = Decision::Clock::time_point{} + seconds(100);
    const auto encodeAt = origin + microseconds(16667);
    Decision decision;
    fixture::require(decision.remaining(origin + milliseconds(25), encodeAt, microseconds(8333)) == microseconds(8333), "Late admission enlarged A or started D at E");
    fixture::require(decision.deadline() == origin + microseconds(33333), "T25 E16.667 did not anchor D33.333");
    fixture::require(decision.pending(origin + microseconds(25100)) && decision.pending(origin + milliseconds(27)), "Pending25.1/Ready27 rejected inside original D");
    fixture::require(!decision.pending(origin + microseconds(33333)) && !decision.pending(origin + milliseconds(50)), "Late wake renewed D instead of VIDEO fallback");
    // A replacement source every3ms has only the original remainder; retiring
    // the old SourceTag cannot slide D even if a caller proposes a later E.
    for (unsigned step = 1; step <= 20; ++step) {
        const auto now = origin + milliseconds(25 + step * 3);
        const auto remaining = decision.remaining(now, now + milliseconds(17), microseconds(8333));
        fixture::require(decision.deadline() == origin + microseconds(33333), "Source burst slid the decision anchor");
        fixture::require(remaining == (now < *decision.deadline() ? duration_cast<microseconds>(*decision.deadline() - now) : microseconds::zero()), "Burst bought another optional allowance");
    }
    decision.reset();
    fixture::require(decision.remaining(origin + milliseconds(5), encodeAt, microseconds(8333)) == microseconds(20000) && decision.deadline() == origin + milliseconds(25), "Existing natural E overlap disappeared");
    std::cout << "Decision clock: T25/E16.667/D33.333 Ready27; expiry/burst/natural-overlap passed\n";
}
void cpuAdmissionClockCases() {
    using namespace std::chrono;
    using Decision = agent::DesktopRegionDecision;
    using Cost = agent::DesktopRegionCost;
    const auto origin = Decision::Clock::time_point{} + seconds(100);
    const auto encodeAt = origin + microseconds(16667);
    const auto early = encodeAt - microseconds(752);
    const auto late = early + microseconds(14485);
    constexpr bool gpuEncoder = true, fullCpuPixels = true;
    fixture::require(gpuEncoder && early < encodeAt, "CPU/GPU-encoder regression did not exercise old early-admission route");
    Decision decision;
    agent::DesktopOptionalProbe probe;
    Cost cost(&probe);
    // A real successful preparation history is retained through idle ticks.
    cost.begin(origin, microseconds(8333), agent::DesktopPreparationSource::kCpu, 24576);
    cost.observe(microseconds(500), true, origin + microseconds(500));
    const auto prediction = cost.preparePrediction();
    auto attempt = [&](auto now) {
        if (!agent::detail::regionPreparationReady(fullCpuPixels, true, now, encodeAt)) {
            return false;
        }
        const auto remaining = decision.remaining(now, encodeAt, microseconds(8333));
        cost.begin(now, remaining, agent::DesktopPreparationSource::kCpu, 24576);
        cost.observationWorkUnits(8192);
        return cost.admit(now, microseconds(15260), microseconds(860));
    };
    fixture::require(!attempt(early) && !attempt(encodeAt - microseconds(1)), "Known CPU source claimed early admission under GPU encoder");
    fixture::require(!decision.deadline() && !probe.last && !cost.admitted() && cost.preparePrediction() == prediction && !cost.within(early), "Idle CPU tick began D/history/probe credit");
    fixture::require(!agent::detail::regionPreparationReady(fullCpuPixels, false, late, encodeAt), "CPU admission bypassed source readiness");
    fixture::require(attempt(late) && cost.admission() == Cost::Admission::kNormal, "Late first CPU decision did not admit immediate known preparation");
    fixture::require(decision.deadline() == late + microseconds(8333) && !probe.last && cost.preparePrediction() == prediction, "Immediate CPU admission erased history or charged idle lateness");
    cost.observationWorkUnits(24576);
    cost.finish(late + microseconds(600), true, microseconds::zero(), microseconds(600));
    fixture::require(cost.preparePrediction() == microseconds(600), "Successful CPU tail was cleared to force normal admission");

    // Cold work also claims its one shared probe only at immediate preparation.
    Decision coldDecision;
    agent::DesktopOptionalProbe coldProbe;
    Cost cold(&coldProbe);
    if (agent::detail::regionPreparationReady(fullCpuPixels, true, early, encodeAt)) {
        cold.begin(early, coldDecision.remaining(early, encodeAt, microseconds(8333)));
        cold.admit(early, microseconds(15260), std::nullopt);
    }
    fixture::require(!coldDecision.deadline() && !coldProbe.last && !cold.preparePrediction() && !cold.admitted(), "Cold CPU idle tick consumed probe/anchor");
    cold.begin(late, coldDecision.remaining(late, encodeAt, microseconds(8333)), agent::DesktopPreparationSource::kCpu, 24576);
    cold.observationWorkUnits(8192);
    fixture::require(cold.admit(late, microseconds(15260), std::nullopt) && cold.admission() == Cost::Admission::kProbe && coldProbe.last == late, "Immediate cold CPU preparation lost shared probe");
    // Actual CPU work that crosses D remains expensive; no failure forgiveness,
    // work-unit rewrite, successful-history reset or cooldown/probe refund.
    const auto slowFinish = late + microseconds(13747);
    cold.finish(slowFinish, false, microseconds::zero(), microseconds(13747));
    fixture::require(cold.preparePrediction() == microseconds(41241) && coldProbe.last == slowFinish, "Real failed CPU sample/work units were washed away");
    fixture::require(coldDecision.remaining(slowFinish, slowFinish + seconds(1), microseconds(8333)) == microseconds::zero() && coldDecision.deadline() == late + microseconds(8333), "Started CPU work renewed D after overrun");
    cold.begin(slowFinish + microseconds(100), microseconds(8333), agent::DesktopPreparationSource::kCpu, 24576);
    fixture::require(!cold.admit(slowFinish + microseconds(100), microseconds(15260), microseconds(860)) && cold.fitDenied() && cold.advantageDenied() && coldProbe.last == slowFinish,
                     "CPU failure tail/cooldown/shared probe disappeared on the next decision");

    // A genuinely unmaterialized ROI still starts before E and may remain
    // Pending under exactly the original anchored D across source/tick changes.
    fixture::require(agent::detail::regionPreparationReady(false, false, early, encodeAt), "True GPU ROI lost early copy overlap");
    Decision roi;
    fixture::require(roi.remaining(early, encodeAt, microseconds(8333)) == microseconds(9085) && roi.deadline() == encodeAt + microseconds(8333), "ROI early admission changed original D");
    fixture::require(roi.pending(encodeAt + microseconds(8000)), "ROI Pending lost its original decision window");
    fixture::require(roi.remaining(late, late + seconds(1), microseconds(8333)) == microseconds::zero() && roi.deadline() == encodeAt + microseconds(8333), "Late ROI/source replacement slid D");
    std::cout << "CPU admission clock: GPU encoder/fullCPU idle has no D/probe/history; immediate late preparation admits; real slow sample retained; ROI D unchanged\n";
}
void initialSnapshotCase() {
    fixture::reset(false);
    fixture::boundedOrdering = true;
    fixture::zeroInitialPresent = true;
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        video.capabilities(caps);
        const auto first = co_await baseline(video);
        const auto counters = video.diagnostics();
        require(first.frameId == 1 && !first.frame.data().empty() && first.sourcePresentTicks == 0, "Zero-present snapshot lost its real initial baseline");
        require(counters.captureCalls >= 1 && counters.captureTimeouts == counters.captureCalls - 1 && counters.returnedSnapshots == 1 && counters.pixelUpdates == 0 && counters.cursorOnlyUpdates == 1,
                "Initial snapshot fabricated a new DXGI pixel-present event");
        require(!counters.lastJobConsumedReady && counters.lastAcquireEpoch == 42 && counters.lastAcquireSerial == 1 && counters.lastAcquirePresentTicks == 0,
                "Initial snapshot did not retain actual Acquire identity");
        require(counters.encodedFrames == 1 && counters.videoEncodeCalls == 1 && counters.publishedVideoFrames == 1 && counters.rejectedPrivateVideoFrames == 0,
                "Initial snapshot lost private/published VIDEO accounting");
        require(first.frame.has_stage_timing() && !first.frame.stage_timing().has_source_age_microseconds(), "Unknown source-present timestamp fabricated source age");
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    std::cout << "Initial snapshot: returned=1/pixel-present=0/VIDEO=1; Acquire retained, source age unknown\n";
}
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
void fusedComparisonCase(unsigned fault) {
    fixture::reset(false);
    fixture::fusedComparison = true;
    fixture::boundedOrdering = true;
    fixture::smallDamage = true;
    fixture::repeatRgb = true;
    fixture::knownTinyCodec = true; // Existing 12kiB real AVC filler premise, not a new V budget.
    fixture::comparisonFault = fault <= 5 || fault >= 11 ? fault : 0;
    fixture::moveCounter = fault == 32;
    fixture::pendingChecks = fault == 6 ? 1 : 0;
    fixture::stalled = fault == 7;
    fixture::comparisonUnknown = fault == 8;
    fixture::smallDamage = fault != 9;
    fixture::baselineUnavailable = fault == 10;
    fixture::sequence = 1;
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(true);
        caps.set_tile_cache(true);
        video.capabilities(caps);
        auto first = co_await baseline(video);
        fixture::displayed(video, first, false);
        require(fault == 10 ? fixture::baselineCopies == 0 : fixture::baselineCopies == 1 && video.diagnostics().comparison.baselineCommits == 1, "GPU primary did not establish an independent baseline key");
        const auto encodes = video.diagnostics().videoEncodeCalls;
        if (fault >= 7 && fault <= 10) {
            ++fixture::version;
            std::optional<agent::DesktopVideoUpdate> primary;
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (!primary && Clock::now() < until) {
                auto update = co_await video.next();
                if (update && (!update->frame.data().empty() || update->region)) {
                    primary = std::move(update);
                }
            }
            require(primary.has_value(), "Missing baseline/unknown/pending produced no legal primary fallback");
            auto fallback = std::move(*primary);
            auto d = video.diagnostics();
            require(d.comparison.unchanged == 0 && fallback.frameId == first.frameId + 1, "Missing baseline/unknown/pending falsely skipped primary");
            require(fault == 10 ? (fallback.region || d.videoEncodeCalls == encodes + 1) : !fallback.region && d.videoEncodeCalls == encodes + 1, "Fallback did not preserve existing VIDEO/ordinary pixelROI contract");
            if (fault == 7) {
                require(d.comparison.submissions == 1 && d.comparison.finishes == 0 && d.comparison.baselineFailures == 1, "Retired Pending slot finished/reused before physical drain");
                fixture::stalled = false;
                co_await video.next();
                d = video.diagnostics();
                require(d.comparison.finishes == 1 && d.comparison.failures == 1, "Retired comparison drain did not finish cost exactly once");
                co_await video.next();
                require(video.diagnostics().comparison.finishes == 1, "Retired comparison cost finished again on a later dispatch");
            } else {
                require(d.comparison.submissions == 0, "Unknown/overflow/missing baseline submitted a partial proof");
            }
            fixture::displayed(video, fallback, false);
        } else if (fault == 6) {
            ++fixture::version;
            std::optional<agent::DesktopVideoUpdate> fallback;
            const auto until = Clock::now() + std::chrono::seconds(1);
            while (!video.diagnostics().comparison.unchanged && !fallback && Clock::now() < until) {
                const auto checkedBefore = fixture::checked.load();
                auto update = co_await video.next();
                require(fixture::checked - checkedBefore <= 1, "Pending comparison checked twice in one dispatch");
                if (update && !update->frame.data().empty()) {
                    fallback = std::move(update);
                }
            }
            const auto d = video.diagnostics();
            if (fallback) {
                // A real timer request is not an 8ms wake guarantee. Only an
                // observed original-D expiry may justify this VIDEO fallback.
                require(d.regionDecisionExpirations && d.roiRetired && fallback->frameId == first.frameId + 1 && d.comparison.unchanged == 0, "Pending proof escaped D/source checks before VIDEO fallback");
                fixture::displayed(video, *fallback, false);
            } else {
                require(d.comparison.unchanged == 1 && d.videoEncodeCalls == encodes && fixture::baselineCopies == 1, "Ready inside D failed to suppress unchanged primary");
            }
            require(d.comparison.submissions == 1 && d.roiPending >= 1 && d.comparison.lastObservationWorkUnits == 24576 && fixture::fullReads == 0, "Pending comparison lost submitted work/slot evidence");
        } else if (fault) {
            fixture::trueRgbChange = fault >= 11;
            ++fixture::version;
            auto fallback = co_await baseline(video);
            const auto d = video.diagnostics();
            const bool currentMismatch = fault >= 20 && fault <= 28;
            require(!fallback.region && d.comparison.rejectedProofs == (currentMismatch ? 0u : 1u) && d.comparison.unchanged == 0 && d.comparison.proofs == 0 && d.videoEncodeCalls == encodes + 1, "Invalid/partial comparison proof skipped a primary or partially advanced damage");
            require(d.comparison.finishes == 1 && d.comparison.failures == 1 && d.comparison.lastObservationWorkUnits == (fault == 32 ? 40960u : 24576u) && d.comparison.lastMinimumCpu.count() >= 0, "Invalid proof diluted GPU work or double-finished cost");
            require(fallback.frameId == first.frameId + 1 && fixture::baselineCopies == 2 && fixture::baselineRgbHash == fixture::encodedRgbHash, "Rejected proof advanced a partial primary instead of preserving actual source RGB");
            require(d.optional.knownPngProducts == 0 && d.lastReadback && d.lastReadback->retirementReason == (currentMismatch ? agent::DesktopVideoFallbackReason::kIdentityOrRevisionMismatch : agent::DesktopVideoFallbackReason::kProofRejected), "Rejected proof invoked PNG or lost its local failure reason");
            fixture::displayed(video, fallback, false);
        } else {
            for (unsigned serial = 2; serial <= 5; ++serial) {
                fixture::version = serial; // New serial/present; RGB equal, alpha changes.
                const auto until = Clock::now() + std::chrono::seconds(1);
                bool cursor = false;
                while (video.diagnostics().comparison.unchanged < serial - 1 && Clock::now() < until) {
                    const auto checkedBefore = fixture::checked.load();
                    auto update = co_await video.next();
                    require(fixture::checked - checkedBefore <= 1, "Fused same-job checked physical slot twice");
                    if (update) {
                        require(update->frame.data().empty() && !update->region && !update->refinement, "Repeated equal RGB produced codec/PNG/frame traffic");
                        cursor |= update->cursor && update->cursor->x() == static_cast<std::int32_t>(serial);
                    }
                }
                const auto d = video.diagnostics();
                require(d.comparison.unchanged == serial - 1 && cursor, "Exact alpha-only proof failed or cursor stopped");
                require(d.videoEncodeCalls == encodes && fixture::baselineCopies == 1 && d.optional.knownPngProducts == 0, "Unchanged comparison advanced baseline or PNG history");
                require(d.comparison.lastAdmission && d.comparison.lastAdmission->comparison && d.comparison.lastAdmission->messageBytes == 0 && d.comparison.lastAdmission->sendPrediction == std::chrono::microseconds::zero(), "Comparison zero-send was confused with unknown PNG bytes");
                require(d.comparison.finishes == serial - 1 && d.comparison.lastObservationWorkUnits == 24576 && d.comparison.lastGpuBytes == 32896, "Comparison observation ignored submitted GPU pixels or finished twice");
            }
            require(video.diagnostics().optional.probe == 1 && video.diagnostics().comparison.lastAdmission->choice == agent::DesktopRegionCost::Admission::kNormal, "Comparison history stayed cold or acquired extra tickets");
            fixture::opaqueRgb = true;
            fixture::trueRgbChange = true;
            ++fixture::version;
            auto changed = co_await baseline(video);
            require(changed.frameId == first.frameId + 1 && video.diagnostics().videoEncodeCalls == encodes + 1 && fixture::baselineCopies == 2, "One real RGB change did not produce one legal primary/new baseline");
            require(video.diagnostics().comparison.unchanged == 4 && video.diagnostics().optional.knownPngProducts == 0, "Cold comparison acquired a second PNG probe");
            const auto coldDenied = video.diagnostics();
            require(coldDenied.lastAdmission && coldDenied.lastAdmission->region && !coldDenied.lastAdmission->preparePrediction && !coldDenied.lastAdmission->messageBytes && coldDenied.lastAdmission->choice == agent::DesktopRegionCost::Admission::kDenied && coldDenied.lastAdmission->sharedProbeBefore,
                    "Unknown PNG did not reject the already-spent comparison cold ticket");
            require(coldDenied.lastReadback && coldDenied.lastReadback->comparisonBaseline && coldDenied.lastReadback->png,
                    "Comparison-to-PNG denial lost the owned work/baseline association");
            const auto coldPng = *coldDenied.lastReadback->png;
            const auto& comparison = coldDenied.lastReadback->admission;
            require(coldPng.admission == *coldDenied.lastAdmission && coldPng.admission.source == agent::DesktopPreparationSource::kCpu &&
                        coldPng.admission.jobId >= comparison.jobId && coldPng.admission.revision == comparison.revision && coldPng.admission.sourceSerial == comparison.sourceSerial &&
                        coldPng.admission.sourceGeneration == comparison.sourceGeneration && coldPng.admission.composedBase == comparison.composedBase &&
                        comparison.decisionDeadline && coldPng.admission.deadline <= *comparison.decisionDeadline &&
                        !coldPng.preparationAttempted && !coldPng.count && !coldPng.finishCost && !coldPng.cacheCommitted && !coldPng.cacheAborted,
                    "Denied CPU PNG borrowed another job or pretended preparation/finish");
            fixture::displayed(video, changed, false);
            // Only after the existing 1Hz shared probe interval, not a refund or
            // new budget, may the normal comparison hand one cold ticket to PNG.
            asio::steady_timer ticket(io);
            ticket.expires_after(std::chrono::milliseconds(1100));
            co_await ticket.async_wait(asio::use_awaitable);
            fixture::trueRgbChange = false;
            ++fixture::version;
            auto png = co_await primaryOutput(video);
            const auto learned = video.diagnostics();
            require(png.region && png.regionAdmission && png.regionAdmission->admission.choice == agent::DesktopRegionCost::Admission::kProbe && learned.optional.probe == 2 && learned.comparison.lastAdmission->choice == agent::DesktopRegionCost::Admission::kNormal,
                    "Normal comparison did not learn first real PNG with exactly one later cold ticket");
            wire::VideoRefinement decoded;
            *decoded.add_patches() = png.region->patches(0);
            fixture::verifyPixels(decoded, fixture::version, true);
            require(learned.lastReadback && learned.lastReadback->png && learned.lastReadback->comparisonBaseline, "Prepared PNG lost its comparison work");
            const auto heldPng = *learned.lastReadback->png;
            require(heldPng.admission == png.regionAdmission->admission && heldPng.preparationAttempted && heldPng.count == 1 && heldPng.attemptedUnits == 24576 &&
                        heldPng.attempts[0].encoded && heldPng.attempts[0].bytes == png.region->patches(0).png().size() && heldPng.attempts[0].stages.pngEncode &&
                        heldPng.finishCost && heldPng.finishedAt && heldPng.finishSuccess && heldPng.cacheCommitted && !heldPng.cacheAborted && !heldPng.rejectionReasons,
                    "Real one-WIC PNG evidence lost actual units/size/stages/finish/cache transaction");
            const auto held = *png.regionAdmission;
            require(held.admission.preparationPrefix > std::chrono::microseconds::zero() && held.admission.composedBase == png.baseFrameId && held.frame == png.frameId && held.admission.sourceSerial == fixture::version,
                    "First real PNG lost its source-key primary batch/prefix");
            fixture::displayed(video, png, false);
            fixture::trueRgbChange = true;
            ++fixture::version;
            auto normal = co_await primaryOutput(video);
            require(normal.region && normal.regionAdmission && normal.regionAdmission->admission.choice == agent::DesktopRegionCost::Admission::kNormal && video.diagnostics().optional.probe == 2,
                    "Learned PNG history was overwritten by comparison history or consumed another cold ticket");
            const auto normalAdmission = *normal.regionAdmission;
            runtimePrefixPolicyReplay(normalAdmission);
            const auto& snapshot = normalAdmission.admission;
            require(snapshot.preparePrediction && snapshot.sendPrediction && *snapshot.preparePrediction <= snapshot.remainingAtAdmission && snapshot.preparationPrefix + *snapshot.preparePrediction + *snapshot.sendPrediction <= snapshot.videoAlternative * 5 / 4,
                    "Runtime sunk-prefix FIT/whole-alternative advantage association was invalid");
            decoded.clear_patches();
            *decoded.add_patches() = normal.region->patches(0);
            fixture::verifyPixels(decoded, fixture::version, true);
            const auto cachedPng = normal.region->patches(0).png();
            const auto cachedSlot = normal.region->patches(0).cache_slot();
            require(!cachedPng.empty() && cachedSlot, "Position-cache test did not establish a real reliable PNG definition");
            fixture::displayed(video, normal, false);
            fixture::cacheShift = true;
            ++fixture::version;
            auto relocated = co_await primaryOutput(video);
            require(relocated.region && relocated.frameId == normal.frameId + 1 && relocated.region->patches_size() == 1 && relocated.region->patches(0).destination().x() == 128 && relocated.region->patches(0).cache_slot() == cachedSlot && relocated.region->patches(0).png().empty(),
                    "RGB cache hit at another position falsely suppressed changed canvas primary");
            decoded.clear_patches();
            *decoded.add_patches() = relocated.region->patches(0);
            decoded.mutable_patches(0)->set_png(cachedPng);
            fixture::verifyPixels(decoded, fixture::version, true);
            require(held == *png.regionAdmission && normalAdmission == *normal.regionAdmission && relocated.regionAdmission == video.diagnostics().latestPublishedRegion && relocated.regionAdmission->admission.sourceSerial == fixture::version && relocated.regionAdmission->base == normal.frameId,
                    "Later source/cache admissions overwrote held primary batch snapshots");
            require(heldPng.admission == held.admission && heldPng.count == 1 && heldPng.cacheCommitted &&
                        coldPng.admission == *coldDenied.lastAdmission && !coldPng.finishCost,
                    "Later work rewrote retained immutable PNG admission evidence");
            fixture::displayed(video, relocated, false);
            std::cout << "Runtime cold comparison/normal comparison/unknown PNG/one later real-WIC cold PNG/normal PNG/other-position cache: passed; held source-key snapshots immutable\n";
            require(fixture::fullReads == 0 && fixture::comparisons == 0, "Fused proof materialized full CPU pixels or synchronized comparison");
        }
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        const auto d = video.diagnostics();
        readbackDiagnostic(d);
        std::cerr << "fused_case_cause fault=" << fault << " submits=" << d.comparison.submissions << " proofs=" << d.comparison.proofs << " unchanged=" << d.comparison.unchanged << " rejected=" << d.comparison.rejectedProofs << " finishes=" << d.comparison.finishes << " cpu_us=" << d.comparison.lastMinimumCpu.count() << " spent_us=" << d.comparison.lastSpent.count() << " expirations=" << d.regionDecisionExpirations << " retired=" << d.roiRetired << " checks=" << fixture::checked << '\n';
        if (d.lastAdmission) {
            admissionNumeric(*d.lastAdmission);
        }
        std::rethrow_exception(failure);
    }
    std::cout << "Fused Runtime case fault=" << fault << " exact RGB/alpha/source/baseline/zero-send/shared-probe/physical-slot passed\n";
}
void fusedMoveAndBackgroundCase(bool rejectRegion) {
    fixture::reset(false);
    fixture::fusedComparison = fixture::boundedOrdering = fixture::smallDamage = true;
    fixture::knownTinyCodec = true;
    fixture::moveCounter = rejectRegion;
    fixture::repeatRgb = !rejectRegion;
    fixture::sequence = 1;
    fixture::privateVideoGate.entered = false;
    {
        std::lock_guard lock(fixture::privateVideoGate.mutex);
        fixture::privateVideoGate.released = false;
    }
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure, producerFailure;
    std::optional<agent::DesktopVideoDiagnostics> pngDiagnostic;
    bool producerFinished = false;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(rejectRegion);
        caps.set_tile_cache(true);
        video.capabilities(caps);
        auto accepted = co_await baseline(video);
        fixture::displayed(video, accepted, false);
        if (rejectRegion) {
            bool region = false;
            const auto until = Clock::now() + std::chrono::seconds(5);
            ++fixture::version;
            while (!region && Clock::now() < until) {
                auto update = co_await video.next();
                if (!update || (update->frame.data().empty() && !update->region)) {
                    continue;
                }
                auto diagnostic = video.diagnostics();
                if (diagnostic.lastReadback && diagnostic.lastReadback->png) {
                    pngDiagnostic = std::move(diagnostic);
                }
                if (update->region) {
                    const auto& patches = update->region->patches();
                    require(update->region->copies_size() == 1 && patches.size() == 2 && patches[0].destination().x() == 128 && patches[1].destination().x() == 256, "Fused mask0 B lost CopyRect corrective patch beside changed C");
                    wire::VideoRefinement products;
                    for (const auto& patch : patches) {
                        *products.add_patches() = patch;
                    }
                    fixture::verifyPixels(products, fixture::version, true);
                    const auto d = video.diagnostics();
                    require(update->regionAdmission && update->regionAdmission == d.latestPublishedRegion && update->regionAdmission->admission.preparationPrefix.count() > 0 && update->regionAdmission->admission.source == agent::DesktopPreparationSource::kCpu, "Compared REGION lost immutable sunk prefix/CPU admission association");
                    require(update->regionAdmission->frame == update->frameId && update->regionAdmission->base == update->baseFrameId, "Primary REGION sidecar borrowed another frame");
                    require(fixture::fullReads == 0, "CopyRect correction read latest/full image instead of frozen tiles");
                    region = true;
                }
                fixture::displayed(video, *update, false);
                accepted = std::move(*update);
                if (!region) {
                    ++fixture::version;
                }
            }
            if (!region) {
                const auto d = video.diagnostics();
                std::cerr << "fused_roi_miss proofs=" << d.comparison.proofs << " unchanged=" << d.comparison.unchanged << " rejected=" << d.comparison.rejectedProofs
                          << " region_candidates=" << d.regionCandidates << " budget_failures=" << d.regionBudgetFailures << " time_failures=" << d.regionTimeBudgetFailures
                          << " optional_denials=" << d.optionalCostDenials << " filtered_pixels=" << d.filteredPixels << " unchanged_updates=" << d.unchangedPixelUpdates
                          << " decision_waits=" << d.regionDecisionWaits << " expirations=" << d.regionDecisionExpirations << " roi_retired=" << d.roiRetired
                          << " png_evidence=" << (d.lastReadback && d.lastReadback->png.has_value()) << " accepted_frame=" << accepted.frameId << " source=" << fixture::version << '\n';
            }
            require(region, "Fused CopyRect correction never passed unchanged original budget/probe window");
        }
        auto before = video.diagnostics();
        auto copies = fixture::baselineCopies.load();
        fixture::privateBaselineGate = true;
        fixture::targetPrivateRegion = rejectRegion;
        fixture::privateVideoGate.armed = true;
        fixture::trueRgbChange = true;
        ++fixture::version;
        auto produce = [&]() -> asio::awaitable<void> {
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < until) {
                before = video.diagnostics();
                copies = fixture::baselineCopies;
                auto result = co_await video.next();
                if (fixture::privateVideoGate.entered) {
                    require(!result, "Background private primary was published after rejection");
                    co_return;
                }
                if (rejectRegion && result && !result->frame.data().empty()) {
                    // Original-D expiry may legally choose VIDEO. It is NOT the
                    // private REGION under test; publish/ACK it normally, and
                    // snapshot counters again for the next bounded attempt.
                    fixture::displayed(video, *result, false);
                    accepted = std::move(*result);
                    ++fixture::version;
                    continue;
                }
                if (result && (!result->frame.data().empty() || result->region)) {
                    throw std::runtime_error("Expected private primary escaped baseline gate");
                }
            }
            throw std::runtime_error("Private primary did not reach baseline gate");
        };
        asio::co_spawn(attachment.executor(), produce(), [&](std::exception_ptr error) { producerFailure = error; producerFinished = true; });
        asio::steady_timer timer(io);
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!fixture::privateVideoGate.entered && !producerFinished && Clock::now() < until) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        require(fixture::privateVideoGate.entered, "Private primary independent baseline was not queued");
        const auto privateHash = fixture::baselineRgbHash.load();
        require(rejectRegion || (privateHash && privateHash == fixture::encodedRgbHash), "Private baseline RGB was not the real codec source");
        wire::QualityFeedback visibility;
        visibility.set_displayed_generation(accepted.generation);
        visibility.set_displayed_frame(accepted.frameId);
        visibility.set_background(true);
        visibility.set_preference(wire::QualityFeedback::SHARP);
        video.feedback(visibility);
        fixture::privateVideoGate.open();
        while (!producerFinished && Clock::now() < until) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        require(producerFinished, "Background primary producer did not finish");
        if (producerFailure) {
            std::rethrow_exception(producerFailure);
        }
        const auto rejected = video.diagnostics();
        require(rejected.rejectedPrivateJobs == before.rejectedPrivateJobs + 1 && fixture::baselineCopies == copies + 1, "Private primary rejection counted twice or lost its real baseline copy");
        require(rejectRegion ? rejected.videoEncodeCalls == before.videoEncodeCalls : rejected.videoEncodeCalls == before.videoEncodeCalls + 1 && rejected.rejectedPrivateVideoFrames == before.rejectedPrivateVideoFrames + 1, "Rejected REGION/VIDEO conflated actual codec work");
        fixture::privateBaselineGate = fixture::targetPrivateRegion = false;
        visibility.set_background(false);
        video.feedback(visibility);
        auto rebuilt = co_await baseline(video);
        const auto after = video.diagnostics();
        require(rebuilt.configuration && rebuilt.generation > accepted.generation && rebuilt.frameId == 1 && rebuilt.frame.key_frame(), "Background early return erased persistent private-primary reset");
        require(after.comparison.proofs == rejected.comparison.proofs && after.videoEncodeCalls == rejected.videoEncodeCalls + 1 && fixture::baselineCopies == copies + 2 && fixture::baselineClears > 0, "Foreground compared an unsent baseline instead of rebuilding a new primary");
        require(privateHash == fixture::baselineRgbHash && fixture::baselineRgbHash == fixture::encodedRgbHash, "Replacement baseline/codec original-source RGB hash metadata was not conserved");
        fixture::displayed(video, rebuilt, false);
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; fixture::privateVideoGate.open(); video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        const auto d = video.diagnostics();
        readbackDiagnostic(d);
        if (pngDiagnostic) {
            readbackDiagnostic(*pngDiagnostic);
        }
        std::cerr << "private_fused_cause region=" << rejectRegion << " encodes=" << d.videoEncodeCalls << " proofs=" << d.comparison.proofs << " copies=" << fixture::baselineCopies << " rejectedJobs=" << d.rejectedPrivateJobs << " encodedRgbHash=" << fixture::encodedRgbHash << " baselineRgbHash=" << fixture::baselineRgbHash << '\n';
        if (d.lastAdmission) {
            admissionNumeric(*d.lastAdmission);
        }
        std::rethrow_exception(failure);
    }
    std::cout << "Fused " << (rejectRegion ? "CopyRect B0/C1 + private REGION" : "private VIDEO") << " background/foreground durable reset and real codec conservation passed\n";
}
void fusedMoveCoverageCase(unsigned mode) {
    fixture::reset(false);
    fixture::fusedComparison = fixture::boundedOrdering = fixture::smallDamage = fixture::moveCounter = fixture::knownTinyCodec = true;
    fixture::moveMode = mode;
    fixture::sequence = 1;
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(true);
        caps.set_tile_cache(true); // Keep the existing committed-reference semantics; verify genuine definitions below.
        video.capabilities(caps);
        auto first = co_await baseline(video);
        fixture::displayed(video, first, false);
        first = co_await baseline(video, true);
        fixture::displayed(video, first, false);
        bool region = false;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!region && Clock::now() < until) {
            wire::QualityFeedback feedback;
            feedback.set_displayed_generation(first.generation);
            feedback.set_displayed_frame(first.frameId);
            feedback.set_preference(wire::QualityFeedback::SHARP);
            feedback.set_maximum_pending_video_frames(1);
            video.feedback(feedback);
            auto held = co_await baseline(video, true);
            fixture::displayed(video, held, false, false); // A real unACKed PRIMARY closes credit.
            const auto submits = fixture::fusedSubmits.load();
            for (unsigned source = 0; source < mode; ++source) {
                ++fixture::version;
                auto queued = co_await video.next();
                require(!queued || (!queued->region && queued->frame.data().empty()), "Move fixture escaped closed PRIMARY credit");
            }
            // One ordinary closed-credit dispatch with NO new source keeps a
            // single-update overlap distinct from the two-update coalescing.
            // Its existing pace, not a new clock/Sleep/OS request, lets E pass.
            co_await video.next();
            require(fixture::fusedSubmits == submits, "Move capture started proof while PRIMARY credit was closed");
            feedback.set_displayed_frame(held.frameId);
            video.feedback(feedback);
            std::optional<agent::DesktopVideoUpdate> update = co_await primaryOutput(video);
            if (update->region) {
                require(fixture::lastComparisonRegions.size() == 2, "Move comparison candidate count was not the full B/C union");
                for (unsigned index = 0; index < 2; ++index) {
                    const auto& candidate = fixture::lastComparisonRegions[index];
                    require(candidate.x == 128 + index * 128 && candidate.y == 0 && candidate.width == 128 && candidate.height == 64, "Overlapping/coalesced moves did not submit the unique COMPLETE B/C candidate union");
                }
                require(update->region->copies_size() == (mode == 2 ? 0 : 2), "Coalesced moves retained invalid CopyRects or overlap lost its corrective copies");
                require(update->region->patches_size() == (mode == 2 ? 1 : 2), "B0/C1 corrective patch policy disagreed with retained versus coalesced copies");
                wire::VideoRefinement products;
                for (const auto& patch : update->region->patches()) {
                    require(!patch.png().empty(), "New move source unexpectedly used a reference without real WIC evidence");
                    *products.add_patches() = patch;
                }
                fixture::verifyPixels(products, fixture::version, true);
                const auto d = video.diagnostics();
                require(d.comparison.lastObservationWorkUnits == 40960 && d.comparison.lastPixels == 16384 && update->regionAdmission && update->regionAdmission == d.latestPublishedRegion && update->regionAdmission->admission.sourceSerial == fixture::version && update->regionAdmission->base == update->baseFrameId && update->regionAdmission->frame == update->frameId, "Move Runtime batch lost complete GPU work/source-key/primary admission");
                const auto& admission = update->regionAdmission->admission;
                require(admission.preparationPrefix > std::chrono::microseconds::zero() && admission.decisionDeadline && admission.deadline <= *admission.decisionDeadline, "Runtime prefix renewed its original D");
                require(admission.preparePrediction ? *admission.preparePrediction <= admission.remainingAtAdmission : admission.choice == agent::DesktopRegionCost::Admission::kProbe && admission.unknownPrediction,
                        "Runtime PNG FIT either charged sunk comparison twice or fabricated a known cold prediction");
                region = true;
            }
            fixture::displayed(video, *update, false);
            first = std::move(*update);
        }
        require(region && fixture::fullReads == 0, "Move coverage never produced a frozen-source real-WIC REGION");
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        readbackDiagnostic(video.diagnostics());
        std::rethrow_exception(failure);
    }
    std::cout << "Runtime move union mode=" << mode << " overlapping/coalesced candidate coverage, B0/C1 real WIC/source association passed\n";
}
void auxiliaryRevisionCase(unsigned reason) {
    fixture::reset(false);
    fixture::fusedComparison = fixture::boundedOrdering = fixture::smallDamage = fixture::details = fixture::stableText = fixture::knownTinyCodec = true;
    fixture::sequence = 1;
    fixture::privateVideoGate.entered = false;
    {
        std::lock_guard lock(fixture::privateVideoGate.mutex);
        fixture::privateVideoGate.released = false;
    }
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure, producerFailure;
    bool produced = false;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(true);
        caps.set_tile_cache(true);
        video.capabilities(caps);
        auto primary = co_await baseline(video);
        fixture::displayed(video, primary, true);
        primary = co_await baseline(video, true);
        fixture::displayed(video, primary, true);
        ++fixture::version; // A genuine GPU-only equal-RGB snapshot; the boot snapshot carried full CPU pixels.
        auto nextBatch = [&]() -> asio::awaitable<agent::DesktopVideoUpdate> {
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < until) {
                auto update = co_await video.next();
                if (update && update->refinement) {
                    require(update->frame.data().empty() && !update->region && update->frameId == primary.frameId && update->generation == primary.generation, "Auxiliary fixture produced another PRIMARY instead of old-frame detail");
                    co_return std::move(*update);
                }
            }
            const auto d = video.diagnostics();
            std::cerr << "auxiliary_batch_timeout reason=" << reason << " proof=" << d.comparison.unchanged << " batches=" << d.refinementBatches << " published=" << d.latestPublishedRefinement.has_value() << " denials=" << d.optionalCostDenials << " normal=" << d.optional.normal << " probe=" << d.optional.probe << " fullReads=" << fixture::fullReads << '\n';
            if (d.lastAdmission) {
                admissionNumeric(*d.lastAdmission);
            }
            throw std::runtime_error("Auxiliary Runtime could not produce a real accepted batch");
        };
        auto oldBatch = co_await nextBatch();
        require(oldBatch.refinementAdmission == video.diagnostics().latestPublishedRefinement, "Old exact tuple was not actually published/leased");
        const auto oldAssociation = *oldBatch.refinementAdmission;
        wire::QualityFeedback feedback;
        feedback.set_displayed_generation(primary.generation);
        feedback.set_displayed_frame(primary.frameId);
        feedback.set_preference(wire::QualityFeedback::SHARP);
        feedback.set_video_refinement(false);
        video.feedback(feedback);
        feedback.set_video_refinement(true);
        video.feedback(feedback);
        auto wrong = fixture::ack(oldBatch);
        wrong.mutable_desktop_refinement_ack()->set_batch_id(oldBatch.refinement->batch_id() + 1);
        video.refinementAck(wrong);
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            auto update = co_await video.next();
            require(!update || !update->refinement, "Revision retirement or wrong tuple fabricated remoteCleanup/unacked credit");
        }
        video.refinementAck(fixture::ack(oldBatch));
        auto secondBatch = co_await nextBatch();
        require(secondBatch.refinement->batch_id() != oldBatch.refinement->batch_id() && oldAssociation == *oldBatch.refinementAdmission, "Exact old ACK did not clean its lease or overwrote held batch association");
        video.refinementAck(fixture::ack(secondBatch));
        const auto before = video.diagnostics();
        const auto copies = fixture::baselineCopies.load(), clears = fixture::baselineClears.load();
        fixture::privateAuxGate = true;
        fixture::privateVideoGate.armed = true;
        auto produce = [&]() -> asio::awaitable<void> {
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < until) {
                auto update = co_await video.next();
                if (fixture::privateVideoGate.entered) {
                    require(!update || (!update->refinement && !update->region && update->frame.data().empty()), "Revision-rejected private auxiliary escaped owner publication");
                    co_return;
                }
                require(!update || !update->refinement, "Auxiliary gate did not retain its owned private BGRA");
            }
            throw std::runtime_error("Auxiliary Runtime did not reach existing private gate");
        };
        asio::co_spawn(attachment.executor(), produce(), [&](std::exception_ptr error) { producerFailure = error; produced = true; });
        asio::steady_timer control(io);
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!fixture::privateVideoGate.entered && !produced && Clock::now() < until) {
            control.expires_after(std::chrono::milliseconds(1));
            co_await control.async_wait(asio::use_awaitable);
        }
        require(fixture::privateVideoGate.entered, "No owned Ready auxiliary for revision rejection");
        if (reason == 1) {
            caps.set_region_updates(false);
            video.capabilities(caps);
        } else {
            feedback.set_video_refinement(reason != 0);
            feedback.set_preference(reason == 2 ? wire::QualityFeedback::SMOOTH : wire::QualityFeedback::SHARP);
            video.feedback(feedback);
        }
        fixture::privateVideoGate.open();
        while (!produced && Clock::now() < until) {
            control.expires_after(std::chrono::milliseconds(1));
            co_await control.async_wait(asio::use_awaitable);
        }
        require(produced, "Auxiliary private job did not finish rejection");
        if (producerFailure) {
            std::rethrow_exception(producerFailure);
        }
        fixture::privateAuxGate = false;
        const auto rejected = video.diagnostics();
        require(fixture::baselineCopies == copies && fixture::baselineClears == clears && rejected.videoEncodeCalls == before.videoEncodeCalls && rejected.rejectedPrivateVideoFrames == before.rejectedPrivateVideoFrames && rejected.ownerRevision == before.ownerRevision + 1, "Auxiliary-only private rejection cleared accepted PRIMARY source baseline or changed quality implicitly");
        if (reason == 0) {
            control.expires_after(std::chrono::milliseconds(1100));
            co_await control.async_wait(asio::use_awaitable);
            ++fixture::version;
            const auto proofUntil = Clock::now() + std::chrono::seconds(2);
            while (video.diagnostics().comparison.unchanged == rejected.comparison.unchanged && Clock::now() < proofUntil) {
                auto update = co_await video.next();
                require(!update || (!update->region && update->frame.data().empty()), "Aux-only revision rolled accepted primary back into forced VIDEO");
            }
            require(video.diagnostics().comparison.unchanged == rejected.comparison.unchanged + 1 && fixture::baselineCopies == copies && fixture::baselineClears == clears && video.diagnostics().comparison.lastBaselineFrame == primary.frameId, "Aux-only revision did not preserve the actual accepted sourceBaseline/frame key");
        } else {
            auto refreshed = co_await baseline(video);
            require(!refreshed.frame.data().empty() && refreshed.frame.key_frame() && refreshed.frameId == primary.frameId + 1 && fixture::baselineCopies == copies + 1, "Permission/quality reset negative control failed to require a genuine new PRIMARY baseline");
        }
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; fixture::privateVideoGate.open(); video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        readbackDiagnostic(video.diagnostics());
        std::rethrow_exception(failure);
    }
    std::cout << "Runtime auxiliary rejection reason=" << reason << " accepted sourceBaseline/exact old-tuple cleanup/permission-quality negative controls passed (private product=owned ROI, not published PNG batch)\n";
}
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
void finalAuxiliaryOwnerCase(unsigned reason) {
    fixture::reset(false);
    fixture::fusedComparison = fixture::boundedOrdering = fixture::smallDamage = fixture::details = fixture::stableText = fixture::knownTinyCodec = true;
    fixture::sequence = 1;
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    const auto loop = attachment.handle();
    agent::DesktopVideoRuntime video(loop, 0, 2);
    auto gate = std::make_shared<fixture::JobReturnGate>();
    gate->loop = loop;
    auto stopping = loop.on_stop([gate]() -> ruvia::task<void> {
        gate->open();
        co_return;
    });
    std::exception_ptr failure;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(true);
        caps.set_tile_cache(true);
        video.capabilities(caps);
        auto primary = co_await baseline(video);
        fixture::displayed(video, primary, true);
        primary = co_await baseline(video, true); // Already accepted SHARP; the toggle must not change quality.
        fixture::displayed(video, primary, true);
        require(primary.frame.target_frames_per_second() == 30 && primary.frame.target_bitrate() == 24000000 && primary.frame.data().size() >= fixture::kTinyKnownVideoBytes,
                "Final auxiliary fixture changed the 12kiB/24Mbps/30 actual VIDEO premise");
        ++fixture::version; // Genuine equal-RGB GPU snapshot, no full-CPU bypass.
        auto nextBatch = [&]() -> asio::awaitable<agent::DesktopVideoUpdate> {
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (Clock::now() < until) {
                auto update = co_await video.next();
                if (update && update->refinement) {
                    require(update->frame.data().empty() && !update->region && update->generation == primary.generation && update->frameId == primary.frameId && update->refinementAdmission,
                            "Final auxiliary group produced a PRIMARY or unassociated batch");
                    fixture::verifyPixels(*update->refinement, fixture::version);
                    fixture::displayed(video, *update, true);
                    co_return std::move(*update);
                }
                require(!update || (update->frame.data().empty() && !update->region), "Auxiliary fixture unexpectedly replaced accepted PRIMARY");
            }
            throw std::runtime_error("Final auxiliary group could not produce a real PNG batch within 5sec");
        };
        auto oldBatch = co_await nextBatch();
        const auto oldAssociation = *oldBatch.refinementAdmission;
        require(video.diagnostics().latestPublishedRefinement == oldBatch.refinementAdmission, "Old genuine batch has no owner lease");
        wire::QualityFeedback feedback;
        feedback.set_displayed_generation(primary.generation);
        feedback.set_displayed_frame(primary.frameId);
        feedback.set_preference(wire::QualityFeedback::SHARP);
        feedback.set_video_refinement(false);
        video.feedback(feedback);
        feedback.set_video_refinement(true);
        video.feedback(feedback);
        auto wrong = fixture::ack(oldBatch);
        wrong.mutable_desktop_refinement_ack()->set_batch_id(oldBatch.refinement->batch_id() + 1);
        video.refinementAck(wrong);
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            auto update = co_await video.next();
            require(!update || !update->refinement, "Retirement/wrong ACK refunded genuine old-tuple credit");
        }
        video.refinementAck(fixture::ack(oldBatch));
        auto secondBatch = co_await nextBatch();
        require(secondBatch.refinement->batch_id() != oldBatch.refinement->batch_id() && oldAssociation == *oldBatch.refinementAdmission,
                "Exact genuine old ACK failed to release only its lease");
        video.refinementAck(fixture::ack(secondBatch));
        const auto before = video.diagnostics();
        const auto copies = fixture::baselineCopies.load(), clears = fixture::baselineClears.load();
        gate->control = [&, before, copies, clears, feedback, caps, acceptedGeneration = primary.generation, acceptedFrame = primary.frameId, reason]() mutable {
            require(loop.is_current(), "Final PNG control ran outside original owner loop");
            const auto stillAccepted = video.diagnostics();
            require(stillAccepted.ownerRevision == before.ownerRevision && stillAccepted.latestPublishedRefinement == before.latestPublishedRefinement &&
                        stillAccepted.refinementBatches == before.refinementBatches && gate->prepared.refinementBatches == before.refinementBatches + 1 &&
                        gate->prepared.refinementTiles == before.refinementTiles + gate->patches && gate->patches && gate->pngBytes && gate->association &&
                        gate->association->batch.generation == acceptedGeneration && gate->association->batch.frame == acceptedFrame &&
                        gate->association->admission.composedBase == acceptedFrame && gate->association->admission.sourceSerial == fixture::version &&
                        gate->association->admission.framesPerSecond == 30 && gate->association->admission.bitrate == 24000000,
                    "Final PNG gate did not separate worker-prepared product from owner publication");
            require(gate->prepared.videoEncodeCalls == before.videoEncodeCalls && fixture::baselineCopies == copies && fixture::baselineClears == clears,
                    "Preparing auxiliary PNG modified accepted PRIMARY/source baseline");
            if (reason == 1) {
                feedback.set_video_refinement(false); // Revision only, retaining accepted PRIMARY and SHARP.
                video.feedback(feedback);
            } else if (reason == 2) {
                caps.set_region_updates(false); // Permission negative control requires later PRIMARY refresh.
                video.capabilities(caps);
            } else if (reason == 3) {
                feedback.set_preference(wire::QualityFeedback::SMOOTH); // Quality negative control.
                video.feedback(feedback);
            }
            const auto controlled = video.diagnostics();
            const bool controlPreservedOwnerState = controlled.ownerRevision == before.ownerRevision + (reason ? 1u : 0u) &&
                                                    controlled.publishedVideoFrames == before.publishedVideoFrames &&
                                                    controlled.latestPublishedRefinement == before.latestPublishedRefinement && fixture::baselineCopies == copies &&
                                                    fixture::baselineClears == clears;
            if (!controlPreservedOwnerState) {
                std::cerr << "final_aux_owner_state reason=" << reason << " revision=" << before.ownerRevision << "->" << controlled.ownerRevision
                          << " published_video=" << before.publishedVideoFrames << "->" << controlled.publishedVideoFrames
                          << " refinement_unchanged=" << (controlled.latestPublishedRefinement == before.latestPublishedRefinement)
                          << " baseline_copies=" << copies << "->" << fixture::baselineCopies.load() << " baseline_clears=" << clears << "->"
                          << fixture::baselineClears.load() << '\n';
            }
            require(controlPreservedOwnerState,
                    "Final PNG owner control changed more than its expected revision before releasing original tuple");
        };
        fixture::armJobReturn(gate);
        std::optional<agent::DesktopVideoUpdate> completed;
        const auto until = Clock::now() + std::chrono::seconds(5);
        while (!gate->completed && Clock::now() < until) {
            completed = co_await video.next(); // Original runBlocking/asAwaitable completion; joined before inspection.
            if (!gate->completed) {
                require(!completed || !completed->refinement, "Armed final PNG batch escaped job-return gate");
            }
        }
        require(gate->completed && gate->posted, "Final PNG gate did not complete its bounded owner post");
        if (gate->failure) {
            std::rethrow_exception(gate->failure);
        }
        const auto after = video.diagnostics();
        require(after.refinementBatches == before.refinementBatches + 1 && after.refinementTiles == before.refinementTiles + gate->patches &&
                    after.videoEncodeCalls == before.videoEncodeCalls && after.publishedVideoFrames == before.publishedVideoFrames &&
                    after.rejectedPrivateVideoFrames == before.rejectedPrivateVideoFrames && fixture::baselineCopies == copies && fixture::baselineClears == clears && fixture::fullReads == 0,
                "Final auxiliary completion lost prepared count or reset/encoded accepted PRIMARY");
        if (!reason) {
            require(completed && completed->refinement && completed->refinementAdmission == gate->association && after.latestPublishedRefinement == gate->association &&
                        after.rejectedPrivateJobs == before.rejectedPrivateJobs && after.ownerRevision == before.ownerRevision,
                    "No-toggle final PNG positive control was not genuinely published");
            fixture::verifyPixels(*completed->refinement, fixture::version);
            fixture::displayed(video, *completed, true);
            video.refinementAck(fixture::ack(*completed));
        } else {
            require(!completed && after.latestPublishedRefinement == before.latestPublishedRefinement && after.rejectedPrivateJobs == before.rejectedPrivateJobs + 1 &&
                        after.ownerRevision == before.ownerRevision + 1 && gate->association != after.latestPublishedRefinement,
                    "Revision-rejected final PNG batch was published or incorrectly uncounted");
        }
        if (reason <= 1) {
            asio::steady_timer ticket(io);
            ticket.expires_after(std::chrono::milliseconds(1100));
            co_await ticket.async_wait(asio::use_awaitable);
            ++fixture::version;
            const auto proofUntil = Clock::now() + std::chrono::seconds(2);
            while (video.diagnostics().comparison.unchanged == after.comparison.unchanged && Clock::now() < proofUntil) {
                auto update = co_await video.next();
                require(!update || (!update->region && update->frame.data().empty()), "Final aux-only boundary forced a new PRIMARY baseline");
                if (update && update->refinement) {
                    fixture::verifyPixels(*update->refinement, fixture::version);
                    fixture::displayed(video, *update, true);
                    video.refinementAck(fixture::ack(*update));
                }
            }
            const auto proof = video.diagnostics();
            require(proof.comparison.unchanged == after.comparison.unchanged + 1 && proof.comparison.lastBaselineFrame == primary.frameId &&
                        proof.lastReadback && proof.lastReadback->comparisonBaseline && proof.lastReadback->comparisonBaseline->composedBase == primary.frameId &&
                        fixture::baselineCopies == copies && fixture::baselineClears == clears && proof.videoEncodeCalls == before.videoEncodeCalls,
                    "Final PNG boundary did not preserve accepted source-key baseline for subsequent genuine proof");
        } else {
            auto refreshed = co_await baseline(video);
            require(refreshed.frame.key_frame() && refreshed.generation == primary.generation && refreshed.frameId == primary.frameId + 1 && fixture::baselineCopies == copies + 1,
                    "Permission/quality negative control failed to require real PRIMARY refresh");
            fixture::displayed(video, refreshed, true);
        }
        require(oldAssociation == *oldBatch.refinementAdmission, "Final PNG control overwrote held genuine old lease association");
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) {
        failure = error;
        fixture::releaseJobReturn();
        gate->open();
        video.stop();
        attachment.request_stop();
    });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    stopping.reset();
    gate->control = {}; // No callback retains references beyond joined runtime work.
    if (failure) {
        readbackDiagnostic(video.diagnostics());
        std::cerr << "final_aux_failure reason=" << reason << " posted=" << gate->posted << " completed=" << gate->completed << " prepared_patches=" << gate->patches << " png_bytes=" << gate->pngBytes << '\n';
        std::rethrow_exception(failure);
    }
    require(fixture::liveTimers == 0, "Final auxiliary group leaked a Runtime timer reference");
    std::cout << "Final genuine aux PNG owner case=" << reason << " completed\n";
}
#endif
void ordinaryLateExpiryCase() {
    fixture::reset(false);
    fixture::boundedOrdering = fixture::smallDamage = fixture::stalled = true;
    fixture::sequence = 1;
    fixture::validator = std::make_unique<protocol::DesktopChannelValidator>(protocol::DesktopChannelBinding{"fixture", "video", wire::DESKTOP_VIDEO, protocol::ChannelSource::kAgent, false, 2});
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        caps.set_region_updates(true);
        video.capabilities(caps);
        auto first = co_await baseline(video);
        fixture::displayed(video, first, false);
        first = co_await baseline(video, true);
        fixture::displayed(video, first, false);
        ++fixture::version;
        auto fallback = co_await baseline(video);
        const auto d = video.diagnostics();
        require(!fallback.region && !fallback.frame.data().empty() && d.regionDecisionExpirations && d.lastReadback && d.lastReadback->purpose == 1 && d.lastReadback->videoFallbackReason == agent::DesktopVideoFallbackReason::kDecisionExpiredBeforeCheck, "Ordinary stalled work did not fall back with its original-D local reason");
        const auto work = *d.lastReadback;
        unsigned finishes = 0, ready = 0;
        std::optional<std::chrono::microseconds> failureTail;
        for (unsigned index = 0; index < work.count; ++index) {
            const auto& point = work.checkpoints[index];
            if (point.kind == agent::DesktopReadbackCheckpointKind::kCostFinish) {
                ++finishes;
                failureTail = point.finishedPreparePrediction;
                require(point.atOrAfterDeadline && failureTail && *failureTail >= work.admission.window, "Original-D failure history/expensive tail disappeared");
            }
            ready += point.kind == agent::DesktopReadbackCheckpointKind::kReady;
        }
        require(finishes == 1 && ready == 0 && work.submittedWorkUnits == 24576 && work.submittedPixels == 8192, "Ordinary late expiry fabricated Ready or washed/double-finished work");
        fixture::displayed(video, fallback, false);
        fixture::stalled = false;
        co_await video.next();
        const auto drained = video.diagnostics();
        require(drained.lastReadback && drained.lastReadback->admission == work.admission && fixture::submitted == 1, "Ordinary retired physical slot was reused before drain");
        unsigned retired = 0;
        finishes = ready = 0;
        for (unsigned index = 0; index < drained.lastReadback->count; ++index) {
            const auto& point = drained.lastReadback->checkpoints[index];
            finishes += point.kind == agent::DesktopReadbackCheckpointKind::kCostFinish;
            ready += point.kind == agent::DesktopReadbackCheckpointKind::kReady;
            retired += point.kind == agent::DesktopReadbackCheckpointKind::kCheckEnd && point.status == static_cast<unsigned>(agent::DesktopReadbackStatus::kRetired);
        }
        require(finishes == 1 && retired == 1 && ready == 0 && drained.lastReadback->retirementReason == work.retirementReason, "Retired drain re-finished, refunded tail or fabricated late Ready");
        readbackDiagnostic(drained);
        std::cout << "Runtime ordinary late original-D expiry/drain one failed finish tail_us=" << failureTail->count() << " passed\n";
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        readbackDiagnostic(video.diagnostics());
        std::rethrow_exception(failure);
    }
}
void privateVideoCase() {
    fixture::reset(false);
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime video(attachment.handle(), 0, 2);
    std::exception_ptr failure, producerFailure;
    std::optional<agent::DesktopVideoUpdate> published;
    bool producerFinished = false;
    fixture::privateVideoGate.armed = true;
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        video.capabilities(caps);
        asio::co_spawn(attachment.executor(), baseline(video), [&](std::exception_ptr error, agent::DesktopVideoUpdate update) {
            producerFailure = error;
            published = std::move(update);
            producerFinished = true;
        });
        const auto until = Clock::now() + std::chrono::seconds(2);
        asio::steady_timer timer(io);
        while (!fixture::privateVideoGate.entered && Clock::now() < until) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        require(fixture::privateVideoGate.entered, "Private codec frame was not produced before revision change");
        video.setOptimizationContract(0);
        fixture::privateVideoGate.open();
        while (!producerFinished && Clock::now() < until) {
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        require(producerFinished, "Private producer did not finish after revision change");
        if (producerFailure) {
            std::rethrow_exception(producerFailure);
        }
        const auto after = video.diagnostics();
        require(after.videoEncodeCalls == 2 && after.rejectedPrivateJobs >= 1, "Rejected private VIDEO lost real codec/production accounting");
        require(published && published->generation == 2 && published->frameId == 1 && after.encodedFrames == 2 && after.publishedVideoFrames == 1 && after.rejectedPrivateVideoFrames == 1,
                "First publication conflated private VIDEO with the committed baseline");
        require(after.lastJobRevision == after.ownerRevision && after.lastAcquireEpoch == 42 && after.lastAcquireSerial != 0, "Replacement VIDEO retained old job identity");
        video.stop();
        co_await video.next();
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; fixture::privateVideoGate.open(); video.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    std::cout << "Private VIDEO: produced=2/published=1/rejected=1; old revision rejected, next baseline independent\n";
}
#endif
void timerLifecycleCases() {
    using namespace ultraterimal;
    fixture::require(fixture::liveTimers == 0, "Prior Runtime leaked a timer request");
    fixture::reset(false);
    asio::io_context io;
    ruvia::execution_domain attachment(io);
    agent::DesktopVideoRuntime first(attachment.handle(), 0, 2), second(attachment.handle(), 0, 2);
    std::exception_ptr failure;
    const auto begins = fixture::timerBegins.load();
    auto verify = [&]() -> asio::awaitable<void> {
        wire::DesktopVideoCapabilities caps;
        caps.add_codecs(wire::AVC);
        caps.set_desktop_optimization_contract(2);
        first.capabilities(caps);
        second.capabilities(caps);
        auto a = co_await baseline(first);
        auto b = co_await baseline(second);
        fixture::require(fixture::liveTimers == 2 && fixture::timerBegins == begins + 2, "Two foreground Runtime owners did not request independently");
        wire::QualityFeedback feedback;
        feedback.set_displayed_generation(a.generation);
        feedback.set_displayed_frame(a.frameId);
        feedback.set_background(true);
        first.feedback(feedback);
        co_await first.next();
        fixture::require(fixture::liveTimers == 1 && !first.diagnostics().timerResolution.active && first.diagnostics().timerResolution.endResult == 0, "Background did not pair timer end after worker release");
        feedback.set_background(false);
        first.feedback(feedback);
        a = co_await baseline(first);
        fixture::require(fixture::liveTimers == 2 && fixture::timerBegins == begins + 3, "Foreground did not restart one request episode");
        second.setOptimizationContract(0);
        b = co_await baseline(second);
        fixture::require(!b.frame.has_stage_timing() && fixture::liveTimers == 1 && !second.diagnostics().timerResolution.requested, "V2 disable retained a timer or old Config");
        first.stop();
        co_await first.next();
        fixture::require(fixture::liveTimers == 0 && !first.diagnostics().timerResolution.requested, "Stop returned before timer release");
        second.stop();
        co_await second.next();
        // Failures are non-fatal and do not issue a per-frame begin retry.
        for (unsigned fault = 1; fault <= 2; ++fault) {
            fixture::timerFault = fault;
            agent::DesktopVideoRuntime failed(attachment.handle(), 0, 2);
            failed.capabilities(caps);
            auto frame = co_await baseline(failed);
            fixture::require(!frame.frame.data().empty() && failed.diagnostics().timerResolution.requested && !failed.diagnostics().timerResolution.active,
                             "Timer failure prevented normal VIDEO startup");
            const auto status = failed.diagnostics().timerResolution;
            fixture::require(fault == 1 ? status.capabilitiesResult == TIMERR_NOCANDO : status.beginResult == TIMERR_NOCANDO, "Timer failure result was concealed");
            wire::QualityFeedback applied;
            applied.set_displayed_generation(frame.generation);
            applied.set_displayed_frame(frame.frameId);
            failed.feedback(applied);
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
            const auto beforeSubmitted = fixture::submitted.load();
            caps.set_region_updates(true);
            fixture::boundedOrdering = true;
            fixture::smallDamage = true;
            fixture::stalled = true;
            failed.capabilities(caps);
            frame = co_await baseline(failed, true); // Commit the permission barrier.
            applied.set_displayed_generation(frame.generation);
            applied.set_displayed_frame(frame.frameId);
            failed.feedback(applied);
            ++fixture::version;
            if (fixture::version % 3 == 0) {
                ++fixture::version; // This boundary's third source has unknown damage, not an eligible ROI.
            }
            const auto fallback = co_await baseline(failed);
            fixture::require(!fallback.region && !fallback.frame.data().empty() && failed.diagnostics().regionDecisionWaits == 0,
                             "Failed timer request waited for optional REGION or blocked VIDEO");
            fixture::require(fixture::submitted > beforeSubmitted, "Failed timer fallback did not exercise Pending readback");
            fixture::boundedOrdering = false;
            fixture::smallDamage = false;
            fixture::stalled = false;
            caps.set_region_updates(false);
#endif
            failed.stop();
            co_await failed.next();
            fixture::require(!failed.diagnostics().timerResolution.endResult && fixture::liveTimers == 0, "Unsuccessful begin had an unpaired end");
        }
        fixture::timerFault = 0;
    };
    asio::co_spawn(attachment.executor(), verify(), [&](std::exception_ptr error) { failure = error; first.stop(); second.stop(); attachment.request_stop(); });
    io.run();
    io.restart();
    auto retired = asio::co_spawn(attachment.executor(), ruvia::as_awaitable(attachment.join()), asio::use_future);
    io.run();
    retired.get();
    fixture::timerFault = 0;
    if (failure) {
        std::rethrow_exception(failure);
    }
    fixture::require(fixture::timerBegins == fixture::timerEnds && fixture::liveTimers == 0, "Runtime timer episode reference counts were not returned");
    std::cout << "Timer lifecycle: multiRuntime/background/foreground/disable/stop/caps-fail/begin-fail paired\n";
}
// Independent negative: a within-deadline preparation may correctly lose
// VIDEO advantage. Keep its real tail and the worker-wide 1Hz probe; it does
// NOT carry the known-cheap coverage promise or any synthetic encoder size.
void withinDeadlineAdvantageFailure() {
    using namespace std::chrono;
    using Cost = ultraterimal::agent::DesktopRegionCost;
    ultraterimal::agent::DesktopOptionalProbe probe;
    Cost cost(&probe);
    const auto origin = Cost::Clock::time_point{} + seconds(100);
    cost.begin(origin, microseconds(8333), ultraterimal::agent::DesktopPreparationSource::kRoi, 24576);
    fixture::require(cost.admit(origin, microseconds(3800), std::nullopt), "Cold ROI probe was removed");
    cost.observe(microseconds(3600), true, origin + microseconds(3600));
    unsigned probes = 1;
    for (unsigned step = 1; step < 300; ++step) {
        const auto now = origin + microseconds(step * 16667);
        cost.begin(now, microseconds(8333), ultraterimal::agent::DesktopPreparationSource::kRoi, 24576);
        if (cost.admit(now, microseconds(3800), microseconds(319))) {
            fixture::require(cost.admission() == Cost::Admission::kProbe, "Expensive tail was erased to force normal progress");
            ++probes;
            cost.observe(microseconds(120), true, now + microseconds(120));
        } else {
            fixture::require(!cost.fitDenied() && cost.advantageDenied(), "Within-deadline negative lost its advantage rejection");
        }
    }
    fixture::require(probes == 5, "Within-deadline negative bypassed the shared 1Hz probe");
}
namespace {
// Keep every original full-suite entry/argv/assertion, even when compiling the
// isolated runner. maybe_unused only describes that runner's entry selection.
[[maybe_unused]] int productionCoreSuite(int argc, char** argv) {
    try {
        withinDeadlineAdvantageFailure();
        regionDecisionClockCases();
        cpuAdmissionClockCases();
        initialSnapshotCase();
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
        privateVideoCase();
        for (unsigned fault = 0; fault <= 10; ++fault) {
            fusedComparisonCase(fault);
        }
        for (unsigned fault = 11; fault <= 32; ++fault) {
            fusedComparisonCase(fault);
        }
        fusedMoveAndBackgroundCase(false);
        fusedMoveAndBackgroundCase(true);
        fusedMoveCoverageCase(1);
        fusedMoveCoverageCase(2);
        for (unsigned reason = 0; reason < 3; ++reason) {
            auxiliaryRevisionCase(reason);
        }
        ordinaryLateExpiryCase();
#endif
        timerLifecycleCases();
#ifdef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
        if (argc == 2 && std::string_view(argv[1]) == "--ordering-only") {
            for (unsigned mode = 1; mode <= 4; ++mode) {
                run(2, false, false, false, false, 0, mode);
            }
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--exception-cost") {
            run(2, false, false, false, false, 1, 0, false, 0, true);
            run(2, false, false, false, false, 3, 0, false, 0, true);
            return 0;
        }
#endif
#ifdef ULTRATERIMAL_PNG_COST_FIXTURE
        if (argc == 2 && std::string_view(argv[1]) == "--cost-failures") {
            for (unsigned mode = 1; mode <= 3; ++mode) {
                run(2, false, false, false, false, 0, 0, false, mode);
            }
            return 0;
        }
#endif
        if (argc == 2 && std::string_view(argv[1]) == "--tiny-only") {
            run(2, false, false, false, false, 0, 0, true);
            return 0;
        }
        run(2, false, false, false, true);
        run(0, false, false, false, true);
        run(3, false, false, false, true);
#ifndef ULTRATERIMAL_GPU_RUNTIME_FIXTURE
        run(0, true);
        run(0, false);
        run(3, false);
        run(2, false);
        run(2, false, false, true);
#else
        for (unsigned mode = 1; mode <= 4; ++mode) {
            run(2, false, false, false, false, 0, mode);
        }
        run(2, false);
        run(2, false, true);
        run(2, false, false, true);
        for (unsigned fault = 1; fault <= 6; ++fault) {
            run(2, false, false, false, false, fault);
        }
#endif
        // Keep the existing micro-window ROI cases ahead of the new sustained
        // CPU-known case; they must not inherit its renderer/encoder load.
        run(2, false, false, false, false, 0, 0, true);
        require(fixture::liveTimers == 0 && fixture::timerBegins == fixture::timerEnds, "Suite leaked a Runtime timer reference");
        std::cout << "Legacy FIFO/cache, V2 gate/stages, >32 multi-batch detail, exact ACK, retirement and same-source dynamic runtime passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace
int main(int argc, char** argv) {
#if defined(ULTRATERIMAL_DESKTOP_JOB_RETURN_FIXTURE)
    try {
        // Dedicated executable only, never a substitute for the full suite.
        require(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--aux-owner-only"), "Job-return runner only owns the final auxiliary group");
        for (unsigned reason = 0; reason < 4; ++reason) {
            finalAuxiliaryOwnerCase(reason);
        }
        require(fixture::liveTimers == 0 && fixture::timerBegins == fixture::timerEnds, "Final auxiliary suite leaked Runtime timer references");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#else
    return productionCoreSuite(argc, argv);
#endif
}
