#include "include/HdrHistogram.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

using mme::HdrHistogram;

namespace {

// Latency-tuned config used everywhere in the engine: 1 ns .. 1 s, 3 sf.
HdrHistogram make_latency_hist() { return HdrHistogram(1, 1'000'000'000, 3); }

// Relative error a 3-significant-figure HDR histogram is allowed: any
// reported value must be within one sub-bucket (<= 0.1%) of the truth.
bool within_sig_figs(std::int64_t reported, std::int64_t truth, double rel = 0.001) {
    if (truth == 0) return reported == 0;
    double err = static_cast<double>(reported - truth) / static_cast<double>(truth);
    if (err < 0) err = -err;
    return err <= rel;
}

}  // namespace

// ---- Construction guards --------------------------------------------

TEST(HdrHistogram, RejectsBadLowest) {
    EXPECT_THROW(HdrHistogram(0, 1000, 3), std::invalid_argument);
}

TEST(HdrHistogram, RejectsHighestBelowTwiceLowest) {
    EXPECT_THROW(HdrHistogram(10, 15, 3), std::invalid_argument);
}

TEST(HdrHistogram, RejectsBadSignificantFigures) {
    EXPECT_THROW(HdrHistogram(1, 1000, 0), std::invalid_argument);
    EXPECT_THROW(HdrHistogram(1, 1000, 6), std::invalid_argument);
}

// ---- Empty state ----------------------------------------------------

TEST(HdrHistogram, EmptyReportsZero) {
    auto h = make_latency_hist();
    EXPECT_TRUE(h.empty());
    EXPECT_EQ(h.total_count(), 0);
    EXPECT_EQ(h.min(), 0);
    EXPECT_EQ(h.max(), 0);
    EXPECT_EQ(h.value_at_quantile(0.5), 0);
    EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

// ---- Basic recording ------------------------------------------------

TEST(HdrHistogram, SingleValueQuantileIsEquivalent) {
    auto h = make_latency_hist();
    h.record_value(42);
    EXPECT_EQ(h.total_count(), 1);
    EXPECT_EQ(h.min(), 42);
    EXPECT_EQ(h.max(), 42);
    // Any quantile lands in the one populated bucket; equivalent to 42.
    EXPECT_TRUE(h.values_are_equivalent(h.value_at_quantile(0.5), 42));
    EXPECT_TRUE(h.values_are_equivalent(h.value_at_quantile(0.999), 42));
}

TEST(HdrHistogram, MinMaxExactWithinResolution) {
    auto h = make_latency_hist();
    h.record_value(7);
    h.record_value(123456);
    h.record_value(999);
    EXPECT_EQ(h.total_count(), 3);
    // min/max track the raw extremes (recorded separately, exact).
    EXPECT_EQ(h.min(), 7);
    EXPECT_EQ(h.max(), 123456);
}

// ---- Percentile accuracy against a known distribution ---------------

TEST(HdrHistogram, UniformDistributionPercentiles) {
    auto h = make_latency_hist();
    for (std::int64_t v = 1; v <= 10000; ++v) h.record_value(v);
    EXPECT_EQ(h.total_count(), 10000);

    EXPECT_TRUE(within_sig_figs(h.value_at_quantile(0.50), 5000));
    EXPECT_TRUE(within_sig_figs(h.value_at_quantile(0.90), 9000));
    EXPECT_TRUE(within_sig_figs(h.value_at_quantile(0.99), 9900));
    EXPECT_TRUE(within_sig_figs(h.value_at_quantile(0.999), 9990));
}

// p99.9 is the case nearest-rank gets wrong with small N. With 1000
// samples valued 1..1000, the true p99.9 is ~999; HDR must report it
// within resolution rather than snapping to a coarse rank.
TEST(HdrHistogram, P999InterpolatesSmallN) {
    auto h = make_latency_hist();
    for (std::int64_t v = 1; v <= 1000; ++v) h.record_value(v);
    EXPECT_TRUE(within_sig_figs(h.value_at_quantile(0.999), 999, 0.005));
}

// ---- Monotonicity ---------------------------------------------------

TEST(HdrHistogram, QuantilesAreMonotone) {
    auto h = make_latency_hist();
    for (std::int64_t v = 1; v <= 100000; ++v) h.record_value(v);
    const std::int64_t p50 = h.value_at_quantile(0.50);
    const std::int64_t p90 = h.value_at_quantile(0.90);
    const std::int64_t p99 = h.value_at_quantile(0.99);
    const std::int64_t p999 = h.value_at_quantile(0.999);
    EXPECT_LE(p50, p90);
    EXPECT_LE(p90, p99);
    EXPECT_LE(p99, p999);
    EXPECT_LE(p999, h.max());
    EXPECT_GE(p50, h.min());
}

// ---- Out-of-range clamping (latency spikes must not throw) ----------

TEST(HdrHistogram, ClampsOutOfRangeValues) {
    HdrHistogram h(1, 1000, 3);
    EXPECT_NO_THROW(h.record_value(-5));        // clamps to lowest (1)
    EXPECT_NO_THROW(h.record_value(50'000));    // clamps to highest (1000)
    EXPECT_EQ(h.total_count(), 2);
    EXPECT_EQ(h.min(), 1);
    EXPECT_EQ(h.max(), 1000);
}

// ---- Mean -----------------------------------------------------------

TEST(HdrHistogram, MeanApproximatesArithmeticMean) {
    auto h = make_latency_hist();
    for (std::int64_t v = 1; v <= 10000; ++v) h.record_value(v);
    // True mean is 5000.5; HDR mean uses bucket midpoints, within 0.1%.
    EXPECT_NEAR(h.mean(), 5000.5, 5000.5 * 0.001 + 1.0);
}

// ---- Equivalence semantics ------------------------------------------

TEST(HdrHistogram, ValuesWithinResolutionAreEquivalent) {
    auto h = make_latency_hist();
    // At ~1e6 the resolution is ~1e6/2000 ~ 512 ns, so 1'000'000 and
    // 1'000'001 fall in the same sub-bucket.
    EXPECT_TRUE(h.values_are_equivalent(1'000'000, 1'000'001));
    // ... but 1'000'000 and 1'010'000 do not.
    EXPECT_FALSE(h.values_are_equivalent(1'000'000, 1'010'000));
}
