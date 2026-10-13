#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <source_location>
#include <stdexcept>
#include <vector>

#include "features/desktop/refinement.runtime.h"
#include "features/desktop/region_budget.types.h"
#ifndef ULTRATERIMAL_DETAIL_ALGORITHMS_ONLY
#include <wincodec.h>

#include <wrl/client.h>

#include "features/desktop/refinement.transport.h"
#include "features/desktop/tile_cache.transport.h"
#endif

namespace {
void require(bool value, std::source_location location = std::source_location::current()) {
    if (!value) {
        throw std::runtime_error("Refinement pixels or bounds failed at line " + std::to_string(location.line()));
    }
}
void verifyComparisonCosts() {
    using namespace ultraterimal::agent;
    using namespace std::chrono;
    using Admission = DesktopRegionCost::Admission;
    const auto now = DesktopRegionCost::Clock::time_point{} + seconds(10);
    // The original D=now+1000us still has 600us after comparison. Sunk
    // comparison is charged to advantage, never deducted from FIT again.
    DesktopOptionalProbe probe{now};
    DesktopRegionCost png(&probe);
    png.begin(now, microseconds(1000));
    png.observe(microseconds(500), true, now);
    const auto afterComparison = now + microseconds(400);
    png.begin(afterComparison, microseconds(600)); // Runtime supplies the same absolute D.
    require(png.normalCandidateFits(afterComparison, microseconds(1100), microseconds(100), 1));
    require(!png.normalCandidateFits(afterComparison, microseconds(1100), microseconds(100), 1, microseconds(400)));
    require(!png.admit(afterComparison, microseconds(1100), microseconds(100), microseconds(400)));
    require(!png.fitDenied() && png.advantageDenied() && !png.unknownPrediction());
    require(png.admission() == Admission::kDenied);
    require(png.preparePrediction() == microseconds(500) && png.prediction(microseconds(100)) == microseconds(600));
    require(png.within(now + microseconds(999)) && !png.within(now + microseconds(1000)));
    // Default/explicit zero retain normal admission and the old 20% rule.
    require(png.admit(afterComparison, microseconds(1100), microseconds(100)));
    require(png.admission() == Admission::kNormal);
    DesktopRegionCost zero(&probe);
    zero.begin(now, microseconds(1000));
    zero.observe(microseconds(500), true, now);
    require(zero.normalCandidateFits(afterComparison, microseconds(1100), microseconds(100), 1, microseconds(0)));
    require(zero.admit(afterComparison, microseconds(1100), microseconds(100), microseconds(0)));
    require(zero.admission() == png.admission() && zero.prediction() == png.prediction());
    // Observe/finish continue to record only this PNG product, not prefix.
    png.finish(afterComparison + microseconds(500), true);
    require(png.preparePrediction() == microseconds(500));
    require(!png.admit(now + microseconds(901), microseconds(1100), microseconds(100), microseconds(400)));
    require(png.fitDenied()); // Remaining 99us, no renewed deadline.
    require(!png.normalCandidateFits(now + microseconds(1000), microseconds(2000), microseconds(0), 1));
    require(!png.admit(now + microseconds(1000), microseconds(2000), microseconds(0)));

    // Substituting videoCost-prefix is not equivalent under 20% hysteresis.
    DesktopRegionCost enter(&probe);
    enter.begin(now, microseconds(1000));
    enter.observe(microseconds(400), true, now);
    // 400+100+400=900 > 1100*0.8, but (400+100) <= (1100-400)*0.8.
    require(!enter.normalCandidateFits(afterComparison, microseconds(1100), microseconds(100), 1, microseconds(400)));
    require(!enter.admit(afterComparison, microseconds(1100), microseconds(100), microseconds(400)));
    require(!enter.fitDenied() && enter.advantageDenied());
    require(zero.admit(afterComparison, microseconds(2000), microseconds(200)));
    // Selected: 500+200+600=1300 <= 1100*1.25; using V-prefix would deny.
    require(zero.normalCandidateFits(afterComparison, microseconds(1100), microseconds(200), 1, microseconds(600)));
    require(zero.admit(afterComparison, microseconds(1100), microseconds(200), microseconds(600)));
    require(!zero.fitDenied() && !zero.advantageDenied());
    require(zero.preparePrediction() == microseconds(500));

    DesktopOptionalProbe stagesProbe;
    DesktopRegionCost comparison(&stagesProbe), product(&stagesProbe);
    comparison.begin(now, microseconds(1000), DesktopPreparationSource::kRoi);
    // Real no-send comparison; PNG prediction remains unknown, not CPU-derived.
    require(comparison.admit(now, microseconds(1100), microseconds(0)));
    require(comparison.unknownPrediction() && comparison.admission() == Admission::kProbe);
    comparison.finish(afterComparison, true);
    require(comparison.preparePrediction() == microseconds(400));
    product.begin(afterComparison, microseconds(600), DesktopPreparationSource::kRoi);
    require(!product.preparePrediction());
    require(!product.admit(afterComparison, microseconds(1100), std::nullopt, microseconds(400)));
    require(product.unknownPrediction() && !product.fitDenied() && product.admission() == Admission::kDenied);
    require(stagesProbe.last == now); // Stage two cannot consume another probe.
    require(!product.within(now + microseconds(1000)));
    product.begin(now + microseconds(999999), microseconds(1000), DesktopPreparationSource::kCpu);
    require(!product.preparePrediction()); // Neither history borrows the other.
    require(!product.admit(now + microseconds(999999), microseconds(1100), std::nullopt, microseconds(400)));
    product.begin(now + seconds(1), microseconds(1000), DesktopPreparationSource::kCpu);
    require(product.admit(now + seconds(1), microseconds(1100), std::nullopt, microseconds(400)));
    require(product.admission() == Admission::kProbe);
    // A later real PNG-only sample still excludes the earlier comparison.
    product.finish(now + seconds(1) + microseconds(500), true);
    require(product.preparePrediction() == microseconds(500));
    product.begin(now + seconds(1) + microseconds(600), microseconds(1000), DesktopPreparationSource::kRoi);
    require(!product.preparePrediction());
    require(!product.admit(now + seconds(1) + microseconds(600), microseconds(1100), microseconds(0), microseconds(400)));
}
void verifyAlgorithms() {
    using namespace ultraterimal::agent;
    using namespace std::chrono;
    const auto now = DesktopRefinementPlan::Clock::time_point{} + seconds(10);
    DesktopRefinementPlan plan;
    const auto generation = static_cast<std::uint64_t>(1) << 40;
    require(plan.begin(generation, 1, 1024, 512, now));
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
    std::uint64_t previous = 0;
    unsigned batches = 0;
    while (!plan.complete()) {
        const auto slice = plan.nextSlice(now, 32);
        require(slice.size() == 8);
        for (const auto& tile : slice) {
            require(seen.emplace(tile.x, tile.y).second);
            require(tile.width == 128 && tile.height == 64);
        }
        const auto work = *plan.workKey();
        require(!plan.publish(now, work, 9, 100, 65536));
        require(!plan.publish(now, work, 8, 65537, 65536));
        require(!plan.publish(now, work, 8, 100, 65537));
        const auto key = plan.publish(now, work, 8, 4096, 65536, batches == 1 ? 99 : 0);
        require(key && key->generation == generation && key->frame == 1 && key->batch > previous);
        previous = key->batch;
        require(plan.nextSlice(now).empty());
        require(!plan.publish(now, work, 1, 100, 1));
        auto wrong = *key;
        ++wrong.frame;
        require(!plan.acknowledge(now, wrong, DesktopDetailAck::kApplied));
        wrong = *key;
        ++wrong.generation;
        require(!plan.acknowledge(now, wrong, DesktopDetailAck::kApplied));
        wrong = *key;
        ++wrong.batch;
        require(!plan.acknowledge(now, wrong, DesktopDetailAck::kApplied));
        require(plan.acknowledge(now, *key, DesktopDetailAck::kApplied));
        require(!plan.acknowledge(now, *key, DesktopDetailAck::kApplied));
        require(!plan.publish(now, work, 1, 100, 1)); // no repeat publication of a slice
        ++batches;
    }
    require(seen.size() == 64 && batches == 8 && plan.sentTiles() == 64);
    require(!plan.begin(generation, 1, 1024, 512, now));

    // Preemption preserves spatial progress, not a top-left restart; late ACKs
    // cannot release the credit for the replacement baseline.
    require(plan.begin(generation, 2, 1024, 512, now));
    const auto first = plan.nextSlice(now);
    const auto oldWork = *plan.workKey();
    const auto retired = plan.publish(now, oldWork, 8, 100, 65536);
    require(retired.has_value());
    require(plan.begin(generation, 3, 1024, 512, now));
    const auto second = plan.nextSlice(now);
    require(second.front() != first.front());
    require(!plan.publish(now, oldWork, 8, 100, 65536)); // stale prepared work
    const auto current = plan.publish(now, *plan.workKey(), 8, 100, 65536);
    require(current && current->batch > retired->batch);
    require(!plan.acknowledge(now, *retired, DesktopDetailAck::kSuperseded));
    require(plan.outstanding() == current);
    require(!plan.poll(now + milliseconds(1999)));
    require(plan.poll(now + seconds(2)) && plan.paused());
    require(!plan.acknowledge(now + seconds(2), *current, DesktopDetailAck::kApplied));
    require(plan.begin(generation, 4, 1024, 512, now + seconds(2)));
    require(plan.nextSlice(now + seconds(2)).empty()); // frame cannot undo timeout
    require(plan.begin(generation + 1, 1, 1024, 512, now + seconds(2)));
    require(!plan.nextSlice(now + seconds(2)).empty());
    const auto expired = plan.publish(now + seconds(2), *plan.workKey(), 1, 100, 8192);
    require(expired && !plan.acknowledge(now + seconds(4), *expired, DesktopDetailAck::kApplied));

    // Spatial preemption covers all tiles despite only one slice per frame.
    DesktopRefinementPlan fair;
    seen.clear();
    for (unsigned frame = 1; frame <= 8; ++frame) {
        require(fair.begin(1, frame, 1024, 512, now));
        for (const auto& tile : fair.nextSlice(now)) {
            require(seen.emplace(tile.x, tile.y).second);
        }
        fair.retire();
    }
    require(seen.size() == 64);
    require(fair.begin(1, 9, 1024, 512, now));
    const DesktopDetailTile roi{897, 449, 1, 1};
    require(fair.restrictTo(std::span(&roi, 1)));
    unsigned found = 0, rounds = 0;
    while (!fair.complete()) {
        const auto slice = fair.nextSlice(now);
        require(++rounds <= 2); // 64 grid positions / 32, not a pixel scan
        for (const auto& tile : slice) {
            require(tile.x == 896 && tile.y == 448);
            ++found;
        }
    }
    require(found == 1);
    fair.includeCanvas();
    unsigned remainingTiles = 0;
    while (!fair.complete()) {
        for (const auto& tile : fair.nextSlice(now)) {
            require(tile.x != roi.x / 128 * 128 || tile.y != roi.y / 64 * 64);
            ++remainingTiles;
        }
    }
    require(remainingTiles == 63);
    DesktopRefinementPlan packed;
    require(packed.begin(1, 1, 256, 128, now));
    const std::array selected{DesktopDetailTile{128, 64, 128, 64}, DesktopDetailTile{0, 0, 128, 64}};
    require(packed.reserveTiles(selected));
    auto unsent = packed.publish(now, *packed.workKey(), 2, 200, 16384);
    require(unsent.has_value());
    packed.rejectPublication(*unsent);
    require(!packed.outstanding() && packed.sentBytes() == 200 && packed.sentTiles() == 2);
    require(!packed.reserveTiles(selected));
    packed.retire();
    require(!packed.resumeCanvas(1, 1, 512, 128));
    require(packed.resumeCanvas(1, 1, 256, 128));
    require(packed.sentBytes() == 200 && packed.sentTiles() == 2);
    require(packed.nextSlice(now).size() == 2);

    DesktopRefinementPlan portrait;
    require(portrait.begin(1, 1, 2160, 3840, now));
    require(portrait.nextSlice(now).size() == 8);
    require(!portrait.begin(2, 1, 4096, 4096, now));
    DesktopRefinementPlan bounded;
    require(bounded.begin(1, 1, 4096, 2160, now));
    while (!bounded.complete()) {
        auto slice = bounded.nextSlice(now);
        require(!slice.empty());
        auto key = bounded.publish(now, *bounded.workKey(), static_cast<std::uint32_t>(slice.size()), 65536, 65536);
        require(key && bounded.acknowledge(now, *key, DesktopDetailAck::kSuperseded));
    }
    require(bounded.sentBytes() == 8 * 1024 * 1024 && bounded.sentTiles() == 1024);
    DesktopRefinementPlan exhausted;
    require(exhausted.begin(std::numeric_limits<std::uint64_t>::max(), 1, 256, 128, now));
    require(exhausted.nextSlice(now, 1).size() == 1);
    auto lastKey = exhausted.publish(now, *exhausted.workKey(), 1, 100, 8192, std::numeric_limits<std::uint64_t>::max());
    require(lastKey && exhausted.acknowledge(now, *lastKey, DesktopDetailAck::kApplied));
    require(exhausted.nextSlice(now, 1).size() == 1);
    require(!exhausted.publish(now, *exhausted.workKey(), 1, 100, 8192));

    DesktopDetailTokens tokens;
    tokens.configure(now, 10000);
    require(tokens.available(now, 65536) == 0);
    require(tokens.available(now + seconds(1), 65536) == 500); // 50ms occupancy
    require(!tokens.consume(now + seconds(1), 501, 65536));
    require(tokens.consume(now + seconds(1), 400, 65536));
    require(tokens.available(now + seconds(1), 65536) == 100);
    tokens.configure(now + seconds(1), 10000); // no new-frame/reconfigure refill
    require(tokens.available(now + seconds(1), 65536) == 100);
    require(tokens.available(now + milliseconds(1100), 65536) == 300);
    require(tokens.available(now + seconds(60), 123) == 123); // upper bound
    tokens.configure(now + seconds(60), 1000);
    require(tokens.available(now + seconds(60), 65536) == 50);

    // Every RGB pixel is examined: odd rows/columns, both axes, equal-luma
    // chroma and high-palette antialiasing cannot be hidden by sampling.
    std::vector<std::uint8_t> pixels(128 * 64 * 4, 255);
    auto score = [&]() { return classifyDesktopDetail({pixels, 128, 64, 512}); };
    require(!score().candidate);
    for (unsigned y = 0; y < 64; ++y) {
        pixels[(y * 128 + 127) * 4] = 0;
    }
    require(score().candidate && score().horizontalEdges == 64 && score().verticalEdges == 0);
    std::fill(pixels.begin(), pixels.end(), std::uint8_t{255});
    for (unsigned x = 0; x < 128; ++x) {
        pixels[(63 * 128 + x) * 4 + 2] = 0;
    }
    require(score().candidate && score().verticalEdges == 128 && score().horizontalEdges == 0);
    std::fill(pixels.begin(), pixels.end(), std::uint8_t{100});
    for (unsigned x = 0; x < 128; ++x) {
        auto* fringe = pixels.data() + (5 * 128 + x) * 4;
        fringe[1] = 106;
        fringe[2] = 88; // ~equal luminance, faint colored antialias fringe
    }
    require(score().candidate && score().verticalEdges == 256);
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 128; ++x) {
            const auto offset = (y * 128 + x) * 4;
            pixels[offset] = static_cast<std::uint8_t>(x);
            pixels[offset + 1] = static_cast<std::uint8_t>(y);
            pixels[offset + 2] = 100;
            if (x == 3 || y == 5) {
                pixels[offset] = 200;
                pixels[offset + 1] = 50;
                pixels[offset + 2] = 0; // colored AA-like stroke, >96 colors
            }
        }
    }
    require(score().candidate && score().horizontalEdges && score().verticalEdges);
    std::fill(pixels.begin(), pixels.end(), std::uint8_t{0});
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 128; ++x) {
            const auto offset = (y * 128 + x) * 4;
            pixels[offset + 1] = x == 1 ? 0 : 100;
            pixels[offset + 2] = x == 1 ? 197 : 0; // approximately equal luminance
        }
    }
    require(score().candidate);
    require(!classifyDesktopDetail({{}, 128, 64, 512}).candidate);
    std::vector<std::uint8_t> padded(516 * 63 + 512, 255);
    require(DesktopDetailPixels{padded, 128, 64, 516}.valid());
    padded.pop_back();
    require(!DesktopDetailPixels{padded, 128, 64, 516}.valid());

    verifyComparisonCosts();
    DesktopRegionCost cost;
    cost.begin(now, milliseconds(4));
    require(cost.admit(now, milliseconds(2)));
    cost.observe(microseconds(500), true, now);
    require(cost.admit(now, milliseconds(2))); // Known history selects the normal path.
    require(cost.prediction(microseconds(100)) == microseconds(600));
    cost.begin(now + milliseconds(10), milliseconds(4));
    require(cost.admit(now + milliseconds(10), microseconds(450))); // retain at 1.11x
    cost.observe(milliseconds(8), false, now + milliseconds(11));   // PNG failure is costly
    require(cost.prediction() >= milliseconds(8));
    const auto failedCooldown = cost.cooldownUntil();
    require(failedCooldown == now + milliseconds(1011));
    require(cost.cooldownUntil() == failedCooldown && cost.prediction() >= milliseconds(8));
    cost.begin(now + milliseconds(20), milliseconds(4));
    require(!cost.admit(now + milliseconds(20), milliseconds(20))); // cooldown
    cost.begin(now + milliseconds(1011), milliseconds(4));
    require(cost.admit(now + milliseconds(1011), milliseconds(2))); // one slow probe
    require(cost.admitted());
    require(!cost.admit(now + milliseconds(1012), milliseconds(2)));
    cost.finish(now + milliseconds(1013), false); // preparation failure counted
    require(!cost.admitted());
    require(cost.prediction() >= milliseconds(8));
    cost.begin(now + seconds(3), microseconds(200));
    require(!cost.admit(now + seconds(3), milliseconds(20)));
    // Identical 8Mbps/60fps inputs: 8333us is a compute deadline,
    // not a compute+network allowance. No sleep/probe is needed for cheap work.
    DesktopOptionalProbe sharedProbe{now};
    DesktopRegionCost domains(&sharedProbe), detailDomains(&sharedProbe);
    domains.begin(now, microseconds(8333));
    domains.observe(microseconds(500), true, now);
    require(domains.preparePrediction().has_value());
    require(domains.prediction(microseconds(1100)) == microseconds(1600));
    require(domains.admit(now, microseconds(9000), microseconds(1100)));
    require(domains.admission() == DesktopRegionCost::Admission::kNormal);
    require(!domains.fitDenied() && !domains.advantageDenied());
    // Even a network estimate occupying the entire quota does NOT consume
    // compute deadline. It can separately lose the 20% video advantage.
    domains.begin(now, microseconds(8333));
    require(!domains.admit(now, microseconds(7000), microseconds(8333)));
    require(!domains.fitDenied() && domains.advantageDenied());
    for (unsigned i = 1; i <= 50; ++i) {
        const auto tick = now + milliseconds(i * 10);
        domains.begin(tick, microseconds(8333));
        require(domains.admit(tick, microseconds(9000), microseconds(1100)));
        require(domains.admission() == DesktopRegionCost::Admission::kNormal);
        domains.finish(tick + microseconds(500), true);
    }
    // CPU-known is not ROI-known; both paths/classes share one cold probe.
    domains.begin(now + milliseconds(600), microseconds(8333), DesktopPreparationSource::kRoi);
    require(!domains.preparePrediction());
    require(!domains.admit(now + milliseconds(600), microseconds(9000), microseconds(1100)));
    domains.begin(now + seconds(1), microseconds(8333), DesktopPreparationSource::kRoi);
    require(domains.admit(now + seconds(1), microseconds(9000), std::nullopt));
    require(domains.admission() == DesktopRegionCost::Admission::kProbe);
    detailDomains.begin(now + seconds(1), microseconds(8333));
    require(!detailDomains.admit(now + seconds(1), microseconds(9000), std::nullopt));
    domains.finish(now + seconds(1) + milliseconds(25), false, milliseconds(10), milliseconds(20));
    require(domains.preparePrediction() == milliseconds(20)); // CPU floor, overlap only once.
    const auto roiCooldown = domains.cooldownUntil();
    require(roiCooldown == now + seconds(2) + milliseconds(25));
    domains.begin(now + milliseconds(1050), microseconds(8333), DesktopPreparationSource::kCpu);
    require(domains.cooldownUntil() == DesktopRegionCost::Clock::time_point{});
    domains.begin(now + milliseconds(1050), microseconds(8333), DesktopPreparationSource::kRoi);
    require(domains.cooldownUntil() == roiCooldown && domains.preparePrediction() == milliseconds(20));
    domains.begin(now + milliseconds(1100), microseconds(8333), DesktopPreparationSource::kRoi);
    require(!domains.admit(now + milliseconds(1100), microseconds(9000), microseconds(20000)));
    require(domains.fitDenied() && domains.advantageDenied());
    domains.begin(now + milliseconds(1100), microseconds(8333), DesktopPreparationSource::kCpu);
    require(domains.preparePrediction() == microseconds(500));
    require(domains.admit(now + milliseconds(1100), microseconds(9000), microseconds(1100)));
    // Changing call scale scales preparation, not a quota; a short tail cannot
    // be reset by begin, a generation change, or a source bucket switch.
    domains.begin(now + milliseconds(1200), microseconds(8333), DesktopPreparationSource::kCpu, 20);
    require(domains.preparePrediction() == microseconds(10000));
    require(!domains.admit(now + milliseconds(1200), microseconds(9000), microseconds(1100)));
    require(domains.fitDenied());
    DesktopRegionCost shortSlice;
    shortSlice.begin(now, microseconds(8333), DesktopPreparationSource::kCpu, 139264); // Predict 8.
    shortSlice.observationWorkUnits(24576);                                            // Only one was actually attempted.
    shortSlice.finish(now + milliseconds(6), false);
    shortSlice.begin(now + seconds(1), microseconds(8333), DesktopPreparationSource::kCpu, 24576);
    require(shortSlice.preparePrediction() == milliseconds(6)); // Not 1059us.
    const auto sizingAt = now + seconds(2);
    shortSlice.begin(sizingAt, microseconds(8333), DesktopPreparationSource::kCpu, 139264);
    require(!shortSlice.normalCandidateFits(sizingAt, microseconds(9000), microseconds(1100), 139264));
    require(shortSlice.normalCandidateFits(sizingAt, microseconds(9000), microseconds(1100), 24576));
    shortSlice.predictionWorkUnits(24576); // Sizing a prefix cannot renew its deadline/probe.
    require(shortSlice.admit(sizingAt, microseconds(9000), microseconds(1100)));
    require(shortSlice.admission() == DesktopRegionCost::Admission::kNormal);
    require(!shortSlice.within(sizingAt + microseconds(8333)));
    require(DesktopStageDurations::microseconds(now, now) == 0);
    require(DesktopStageDurations::microseconds(now, now + seconds(70)) == 60000000);
    require(DesktopStageDurations::microseconds(now, now - seconds(1)) == 0);
    require(!DesktopStageDurations{}.pngEncode);
    DesktopRegionCost overlap;
    overlap.begin(now, milliseconds(60));
    overlap.finish(now + milliseconds(25), true, milliseconds(20));
    require(overlap.prediction() == milliseconds(5));
    DesktopRegionCost sunk;
    sunk.begin(now, milliseconds(60));
    sunk.finish(now + milliseconds(25), false, milliseconds(25), milliseconds(6));
    require(sunk.prediction() == milliseconds(6));
    sunk.begin(now + milliseconds(26), milliseconds(60));
    require(!sunk.admit(now + milliseconds(26), milliseconds(20))); // failure retains cooldown
    DesktopRegionCost hysteresis;
    hysteresis.begin(now, milliseconds(4));
    hysteresis.observe(microseconds(1000), true, now);
    require(hysteresis.admit(now, microseconds(1100)));  // initial probe, not selected
    require(!hysteresis.admit(now, microseconds(1100))); // <20% advantage
    require(hysteresis.admit(now, microseconds(1300)));  // enter at >20%
    require(hysteresis.admit(now, microseconds(1100)));  // remain with hysteresis
    require(!hysteresis.admit(now, microseconds(700)));  // video materially cheaper
    for (unsigned index = 0; index < 8; ++index) {
        cost.observe(microseconds(300), true, now + seconds(4));
    }
    require(cost.prediction() < milliseconds(2)); // finite tail expires, EWMA adapts
}
} // namespace
#ifdef ULTRATERIMAL_DETAIL_ALGORITHMS_ONLY
int main() {
    try {
        verifyAlgorithms();
        std::cout << "Bounded detail planner, exact ACK, RGB edges, tokens and cost admission passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#else
int main() {
    using namespace ultraterimal;
    using Microsoft::WRL::ComPtr;
    try {
        verifyAlgorithms();
        std::vector<std::uint8_t> pixels(256 * 128 * 4, 255);
        for (unsigned y = 0; y < 128; ++y) {
            for (unsigned x = 0; x < 256; ++x) {
                const auto offset = (y * 256 + x) * 4;
                if ((x % 16) < 3 && y % 16 < 10) {
                    pixels[offset] = 10;
                    pixels[offset + 1] = 20;
                    pixels[offset + 2] = 230;
                }
            }
        }
        auto detail = agent::refineVideoDetails(pixels, 256, 128);
        require(detail.patches_size() == 4);
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(initialized));
        struct Apartment {
            ~Apartment() { CoUninitialize(); }
        } apartment;
        ComPtr<IWICImagingFactory> factory;
        require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))));
        const auto verifyPng = [&](const wire::PatchRect& patch, std::span<const std::uint8_t> source, unsigned sourceWidth, unsigned originX, unsigned originY) {
            ComPtr<IWICStream> stream;
            require(SUCCEEDED(factory->CreateStream(&stream)));
            require(SUCCEEDED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(patch.png().data())), static_cast<DWORD>(patch.png().size()))));
            ComPtr<IWICBitmapDecoder> decoder;
            require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)));
            ComPtr<IWICBitmapFrameDecode> frame;
            require(SUCCEEDED(decoder->GetFrame(0, &frame)));
            const auto& area = patch.destination();
            std::vector<std::uint8_t> restored(area.width() * area.height() * 4);
            require(SUCCEEDED(frame->CopyPixels(nullptr, area.width() * 4, static_cast<UINT>(restored.size()), restored.data())));
            for (unsigned y = 0; y < area.height(); ++y) {
                require(std::memcmp(restored.data() + y * area.width() * 4, source.data() + ((originY + y) * sourceWidth + originX) * 4, area.width() * 4) == 0);
            }
        };
        for (const auto& patch : detail.patches()) {
            verifyPng(patch, pixels, 256, patch.destination().x(), patch.destination().y());
        }
        agent::DesktopTileCache local;
        agent::DesktopDetailTokens tokens;
        agent::DesktopRegionCost cost;
        using Clock = agent::DesktopRegionCost::Clock;
        tokens.configure(Clock::now() - std::chrono::seconds(1), 10 * 1024 * 1024);
        for (unsigned pattern = 0; pattern < 5; ++pattern) {
            std::vector<std::uint8_t> tile(128 * 64 * 4, 255);
            for (unsigned y = 0; y < 64; ++y) {
                for (unsigned x = 0; x < 128; ++x) {
                    const auto offset = (y * 128 + x) * 4;
                    if (pattern == 4) { // antialiasing/high palette, no color gate
                        tile[offset] = static_cast<std::uint8_t>(x);
                        tile[offset + 1] = static_cast<std::uint8_t>(y);
                        tile[offset + 2] = static_cast<std::uint8_t>((x + y) % 256);
                    }
                    if ((pattern == 0 && x == 3) || (pattern == 1 && y == 5) ||
                        (pattern == 2 && x == 127) || (pattern == 3 && y == 63) ||
                        (pattern == 4 && x % 13 == 3 && y % 17 < 11)) {
                        tile[offset] = 197;
                        tile[offset + 1] = 40;
                        tile[offset + 2] = 16;
                    }
                }
            }
            cost.begin(Clock::now(), std::chrono::seconds(60));
            auto attempt = agent::prepareVideoDetail({tile, 128, 64, 512}, {512, 256, 128, 64}, local, tokens, cost, 65536,
                                                     std::chrono::seconds(1), std::chrono::milliseconds(1));
            require(attempt.admitted && attempt.candidate && attempt.patch && attempt.stages.prepareCpu && attempt.stages.pngEncode);
            require(attempt.patch->cache_slot() == 0 && !attempt.patch->png().empty());
            require(attempt.patch->destination().x() == 512 && attempt.patch->destination().y() == 256);
            verifyPng(*attempt.patch, tile, 128, 0, 0);
            cost.begin(Clock::now(), std::chrono::seconds(60));
            auto reused = agent::prepareVideoDetail({tile, 128, 64, 512}, {512, 256, 128, 64}, local, tokens, cost, 65536,
                                                    std::chrono::seconds(1), std::chrono::milliseconds(1));
            require(reused.patch && !reused.patch->png().empty() && reused.patch->cache_slot() == 0 && !reused.stages.pngEncode);
            verifyPng(*reused.patch, tile, 128, 0, 0);
            if (pattern == 0) {
                // Simulate an admission preceding optional platform work. PNG
                // must reuse a slow probe, not attempt to acquire it twice.
                agent::DesktopRegionCost preparedCost;
                auto beforePlatform = Clock::now();
                preparedCost.begin(beforePlatform, std::chrono::seconds(60));
                preparedCost.observe(std::chrono::milliseconds(100), true, beforePlatform);
                preparedCost.begin(beforePlatform, std::chrono::seconds(60));
                require(preparedCost.admit(beforePlatform, std::chrono::milliseconds(1)) && preparedCost.admitted());
                auto prepared = agent::prepareVideoDetail({tile, 128, 64, 512}, {512, 256, 128, 64}, local, tokens, preparedCost, 65536,
                                                          std::chrono::milliseconds(1), std::chrono::microseconds(0));
                require(prepared.patch && !preparedCost.admitted());
                verifyPng(*prepared.patch, tile, 128, 0, 0);
            }
        }
        agent::DesktopTileCache failing;
        std::vector<std::uint8_t> failureTile(128 * 64 * 4, 255);
        for (unsigned y = 0; y < 64; ++y) {
            failureTile[(y * 128 + 3) * 4] = 0;
        }
        cost.begin(Clock::now(), std::chrono::seconds(60));
        auto failed = agent::prepareVideoDetail({failureTile, 128, 64, 512}, {0, 0, 128, 64}, failing, tokens, cost, 65,
                                                std::chrono::seconds(1), std::chrono::milliseconds(1));
        require(failed.admitted && failed.candidate && !failed.patch && failed.stages.pngEncode);
        cost.begin(Clock::now(), std::chrono::seconds(60));
        require(!cost.admit(Clock::now(), std::chrono::seconds(1))); // failed PNG/copy incurred cooldown
        wire::Envelope message;
        message.set_protocol_version(protocol::kVersion);
        message.set_session_id("s");
        message.set_channel_id("v");
        message.set_sequence(1);
        message.set_generation(1);
        message.set_frame_id(1);
        message.set_base_frame_id(1);
        *message.mutable_video_refinement() = detail;
        require(protocol::validateMessage(message).has_value());
        message.mutable_video_refinement()->mutable_patches(0)->mutable_destination()->set_width(129);
        require(!protocol::validateMessage(message));
        std::fill(pixels.begin(), pixels.end(), std::uint8_t{255});
        require(agent::refineVideoDetails(pixels, 256, 128).patches_size() == 0);
        require(agent::refineVideoDetails({}, 256, 128).patches_size() == 0);
        std::cout << "Lossless detail PNGs preserve exact pixels; flat images and oversized input rejected\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
