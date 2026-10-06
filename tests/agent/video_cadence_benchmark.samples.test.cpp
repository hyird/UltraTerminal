#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "video_cadence_benchmark.samples.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("Cadence benchmark sample assertion failed");
    }
}
} // namespace
int main() {
    using namespace ultraterimal::test::cadence;
    Samples empty;
    require(!empty.mean() && !empty.peak() && !empty.percentile(99));
    Samples samples;
    for (unsigned value = 0; value < 3600; ++value) {
        samples.observe(value * 1000);
    }
    require(samples.count() == 3600 && samples.dropped() == 0);
    require(samples.mean() == 1799500 && samples.percentile(95) == 3419000 && samples.percentile(99) == 3563000);
    require(samples.peak() == 3599000 && samples.ge100ms() == 3500);
    Samples overflow(2);
    overflow.observe(0);
    overflow.observe(100000);
    overflow.observe(200000);
    require(overflow.count() == 3 && overflow.dropped() == 1 && overflow.mean() == 100000 && overflow.peak() == 200000);
    require(!overflow.percentile(95) && overflow.ge100ms() == 2);
    Stages stages, staticStages;
    Timing primary{}, dynamic{}, stationary{};
    primary[0] = 0;
    dynamic[0] = 900;
    dynamic[7] = 200;
    stationary[7] = 800;
    stages.observe(primary, dynamic);
    staticStages.observe(stationary);
    require(stages.samples[0].count() == 1 && stages.samples[0].mean() == 0);
    require(stages.samples[7].count() == 1 && stages.samples[7].mean() == 200);
    require(staticStages.samples[7].count() == 1 && staticStages.samples[7].mean() == 800);
    stages.observe(primary, dynamic); // Distinct updates, even on the same source timestamp.
    require(stages.samples[0].count() == 2);
    Coverage coverage;
    coverage.observe(1, 10, 0, 0, 128 * 33, 64);
    coverage.observe(1, 10, 0, 0, 128, 64);
    require(coverage.unique() == 33 && coverage.maximumPerBase() == 33);
    coverage.observe(2, 10, 0, 0, 128 * 32, 64);
    require(coverage.unique() == 65 && coverage.maximumPerBase() == 33);
    Coverage differentBases;
    differentBases.observe(1, 10, 0, 0, 128 * 32, 64);
    differentBases.observe(1, 11, 0, 0, 128 * 32, 64);
    require(differentBases.unique() == 64 && differentBases.maximumPerBase() == 32);
    Coverage crossing;
    crossing.observe(1, 1, 127, 63, 2, 2);
    require(crossing.unique() == 4);
    Coverage bounded(2);
    bounded.observe(1, 1, 0, 0, 128 * 3, 64);
    require(bounded.overflow() && !bounded.unique() && !bounded.maximumPerBase());
    require(counterDelta(0, 0) == 0);
    require(counterDelta(0, 9007199254740993ULL) == 9007199254740993ULL);
    require(counterDelta(1, std::numeric_limits<std::uint64_t>::max()) == std::numeric_limits<std::uint64_t>::max() - 1);
    require(!counterDelta(2, 1));
    std::ostringstream deltas;
    integer(deltas, counterDelta(0, 0));
    deltas << ',';
    integer(deltas, counterDelta(2, 1));
    deltas << ',';
    integer(deltas, counterDelta(0, 9007199254740993ULL));
    deltas << ',';
    integer(deltas, counterDelta(0, std::numeric_limits<std::uint64_t>::max()));
    require(deltas.str() == "0,null,9007199254740993,18446744073709551615");
    require(!qualifiedAssociation(0, 0, std::nullopt, true));
    // Warmup/crossing/final next, then a retained warmup snapshot.
    require(!qualifiedAssociation(9, 8, std::nullopt, false));
    require(!qualifiedAssociation(9, 9, std::nullopt, true));
    require(qualifiedAssociation(9, 8, std::nullopt, true));
    require(qualifiedAssociation(9, 9, 9, true));
    require(!qualifiedAssociation(10, 10, 9, true));
    // Older work is not new; final next cannot qualify even a previously seen ID.
    require(!qualifiedAssociation(8, 9, std::nullopt, true));
    require(!qualifiedAssociation(9, 9, 9, false));
    require(boundedCheckpointCount(0) == 0 && boundedCheckpointCount(32) == 32);
    require(boundedCheckpointCount(33) == 32 && boundedCheckpointCount(std::numeric_limits<std::uint32_t>::max()) == 32);
    const auto paintFixture = [] {
        PaintMeasurement value;
        value.frequency = 990;
        require(value.observe({10, 100, 110}, 0, 1, 1000));
        require(value.observe({20, 200, 210}, 2, 3, 1000));
        return value;
    };
    const auto measuredPaint = paintFixture();
    require(measuredPaint.qualified() && measuredPaint.countDelta() == 10);
    require(measuredPaint.minimumTicks() == 90 && measuredPaint.maximumTicks() == 110);
    require(measuredPaint.rate(false) == 90 && measuredPaint.rate(true) == 110 && measuredPaint.estimatedRate() == 99);
    auto invalidPaint = measuredPaint;
    invalidPaint.frequency.reset();
    require(!invalidPaint.qualified() && !invalidPaint.countDelta() && !invalidPaint.estimatedRate());
    invalidPaint.frequency = 0;
    require(!invalidPaint.qualified());
    invalidPaint = measuredPaint;
    invalidPaint.begin->qpcBefore.reset();
    require(!invalidPaint.countDelta());
    invalidPaint = measuredPaint;
    invalidPaint.end->qpcAfter.reset();
    require(!invalidPaint.countDelta());
    invalidPaint = measuredPaint;
    invalidPaint.end->count.reset();
    require(!invalidPaint.countDelta());
    invalidPaint = measuredPaint;
    invalidPaint.end->count = 9;
    require(!invalidPaint.countDelta());
    invalidPaint = measuredPaint;
    invalidPaint.begin->qpcAfter = 99;
    require(!invalidPaint.qualified());
    invalidPaint = measuredPaint;
    invalidPaint.end->qpcBefore = 211;
    require(!invalidPaint.qualified());
    invalidPaint = measuredPaint;
    invalidPaint.end->qpcBefore = 109;
    require(!invalidPaint.qualified());
    invalidPaint = measuredPaint;
    invalidPaint.end->windowBeforeNanoseconds = 0;
    require(!invalidPaint.qualified());
    auto touchingPaint = measuredPaint;
    touchingPaint.end->qpcBefore = 110;
    require(touchingPaint.qualified() && touchingPaint.minimumTicks() == 0);
    require(touchingPaint.rate(false) == 90 && !touchingPaint.rate(true));
    PaintMeasurement emptyPaint;
    require(!emptyPaint.qualified() && !emptyPaint.countDelta());
    emptyPaint.frequency = 1;
    require(!emptyPaint.observe({0, 0, 0}, -1, 0, 1000));
    require(!emptyPaint.observe({0, 0, 0}, 1, 0, 1000));
    require(!emptyPaint.observe({0, 0, 0}, 0, 0, 0));
    require(!emptyPaint.observe({0, 0, 0}, 999, 1000, 1000));
    require(!emptyPaint.observe({0, 0, 0}, 1000, 1001, 1000));
    require(!emptyPaint.begin && !emptyPaint.end); // Warmup/final/boundary never stored.
    require(emptyPaint.observe({0, 0, 0}, 0, 0, 1000));
    require(!emptyPaint.qualified()); // One zero-duration read is not an interval.
    require(emptyPaint.observe({0, 1, 1}, 999, 999, 1000));
    require(emptyPaint.qualified() && emptyPaint.countDelta() == 0 && emptyPaint.estimatedRate() == 0);
    auto largePaint = measuredPaint;
    largePaint.begin->count = 0;
    largePaint.end->count = 9007199254740993ULL;
    require(largePaint.countDelta() == 9007199254740993ULL);
    std::ostringstream paintJson;
    paintJson << "{\"test\":true";
    largePaint.write(paintJson);
    paintJson << '}';
    require(paintJson.str().find("\"callback_count_delta\":9007199254740993") != std::string::npos);
    require(paintJson.str().find("\"qualified\":true,\"reason\":null") != std::string::npos);
    largePaint.end->count = std::numeric_limits<std::uint64_t>::max();
    require(largePaint.countDelta() == std::numeric_limits<std::uint64_t>::max());
    const auto maximumTick = std::numeric_limits<std::uint64_t>::max();
    largePaint.begin->qpcBefore = maximumTick - 110;
    largePaint.begin->qpcAfter = maximumTick - 100;
    largePaint.end->qpcBefore = maximumTick - 10;
    largePaint.end->qpcAfter = maximumTick;
    require(largePaint.minimumTicks() == 90 && largePaint.maximumTicks() == 110);
    std::ostringstream maximumPaintJson;
    largePaint.write(maximumPaintJson);
    require(maximumPaintJson.str().find("\"callback_count_delta\":18446744073709551615") != std::string::npos);
    require(maximumPaintJson.str().find("\"qpc_after_read\":18446744073709551615") != std::string::npos);
    std::ostringstream absentPaintJson;
    PaintMeasurement{}.write(absentPaintJson);
    require(absentPaintJson.str().find("\"qualified\":false,\"reason\":\"unknown\"") != std::string::npos);
    require(absentPaintJson.str().find("\"qpc_frequency\":null,\"begin\":null,\"end\":null,\"callback_count_delta\":null") != std::string::npos);
    std::ostringstream zeroPaintJson;
    emptyPaint.write(zeroPaintJson);
    require(zeroPaintJson.str().find("\"callback_count_delta\":0") != std::string::npos);
    std::ostringstream integers;
    integers << "\"integer_zero\":";
    integer(integers, std::optional<std::uint64_t>(0));
    integers << ",\"integer_absent\":";
    integer(integers, std::optional<std::uint64_t>{});
    integers << ",\"integer_above_double_precision\":";
    integer(integers, std::optional<std::uint64_t>(9007199254740993ULL));
    integers << ",\"integer_uint64_max\":";
    integer(integers, std::optional(std::numeric_limits<std::uint64_t>::max()));
    integers << ",\"integer_grid\":";
    integer(integers, std::optional<std::size_t>(33));
    integers << ",\"double_zero\":";
    number(integers, std::optional(0.0));
    integers << ",\"double_absent\":";
    number(integers, std::optional<double>{});
    require(integers.str() == "\"integer_zero\":0,\"integer_absent\":null,\"integer_above_double_precision\":9007199254740993,\"integer_uint64_max\":18446744073709551615,\"integer_grid\":33,\"double_zero\":0,\"double_absent\":null");
    std::ostringstream output;
    output << "{\"test\":true," << integers.str();
    stages.write(output);
    staticStages.write(output, "static_detail_");
    output << '}';
    require(output.str().find("\"worker_queue_mean_us\":0") != std::string::npos);
    require(output.str().find("\"acquire_wait_mean_us\":null") != std::string::npos);
    require(output.str().find("\"worker_queue_p99_us\":0") != std::string::npos);
    std::cout << output.str() << '\n';
}
