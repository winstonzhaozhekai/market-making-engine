// Unit tests for the M8 LatencyScheduler primitive: min-heap ordering,
// stable tiebreaker on equal time_ns, clock advance on pop, reuse via
// clear(), and per-stage sampler shape/determinism.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "include/LatencyScheduler.h"

using mme::LatencyDistribution;
using mme::LatencyEvent;
using mme::LatencyEventKind;
using mme::LatencyScheduler;
using mme::StageLatencyConfig;
using mme::StageSampler;

// ---- LatencyScheduler ------------------------------------------------------

TEST(LatencyScheduler, StartsEmpty) {
    LatencyScheduler s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.now_ns(), 0);
}

TEST(LatencyScheduler, OrdersByTime) {
    LatencyScheduler s;
    s.schedule(300, LatencyEventKind::FeedDeliver, 30);
    s.schedule(100, LatencyEventKind::FeedDeliver, 10);
    s.schedule(200, LatencyEventKind::FeedDeliver, 20);

    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.peek_next_time(), 100);

    LatencyEvent e1 = s.pop_next();
    EXPECT_EQ(e1.time_ns, 100);
    EXPECT_EQ(e1.handle,  10u);

    LatencyEvent e2 = s.pop_next();
    EXPECT_EQ(e2.time_ns, 200);
    EXPECT_EQ(e2.handle,  20u);

    LatencyEvent e3 = s.pop_next();
    EXPECT_EQ(e3.time_ns, 300);
    EXPECT_EQ(e3.handle,  30u);

    EXPECT_TRUE(s.empty());
}

TEST(LatencyScheduler, TiebreakerIsSubmissionOrder) {
    // All three events scheduled at t=500ns. Pop order must match push
    // order — otherwise byte-equality across runs dies under heap rebalance.
    LatencyScheduler s;
    s.schedule(500, LatencyEventKind::OrderLand,   1);
    s.schedule(500, LatencyEventKind::FillReport,  2);
    s.schedule(500, LatencyEventKind::FeedDeliver, 3);

    auto a = s.pop_next();
    auto b = s.pop_next();
    auto c = s.pop_next();

    EXPECT_EQ(a.handle, 1u);
    EXPECT_EQ(b.handle, 2u);
    EXPECT_EQ(c.handle, 3u);
}

TEST(LatencyScheduler, NowAdvancesOnPop) {
    LatencyScheduler s;
    s.schedule(100, LatencyEventKind::FeedDeliver, 1);
    s.schedule(250, LatencyEventKind::FeedDeliver, 2);
    s.schedule(700, LatencyEventKind::FeedDeliver, 3);

    EXPECT_EQ(s.now_ns(), 0);
    s.pop_next();
    EXPECT_EQ(s.now_ns(), 100);
    s.pop_next();
    EXPECT_EQ(s.now_ns(), 250);
    s.pop_next();
    EXPECT_EQ(s.now_ns(), 700);
}

TEST(LatencyScheduler, InterleavedScheduleAndPop) {
    LatencyScheduler s;
    s.schedule(100, LatencyEventKind::FeedDeliver, 1);
    s.schedule(500, LatencyEventKind::FeedDeliver, 5);

    auto e = s.pop_next();
    EXPECT_EQ(e.time_ns, 100);

    // After now_ns=100, scheduling at t=200 is legal.
    s.schedule(200, LatencyEventKind::OrderLand, 2);
    s.schedule(400, LatencyEventKind::FillReport, 4);

    e = s.pop_next(); EXPECT_EQ(e.time_ns, 200);
    e = s.pop_next(); EXPECT_EQ(e.time_ns, 400);
    e = s.pop_next(); EXPECT_EQ(e.time_ns, 500);
    EXPECT_TRUE(s.empty());
}

TEST(LatencyScheduler, ClearResetsClockAndSeq) {
    LatencyScheduler s;
    s.schedule(100, LatencyEventKind::FeedDeliver, 1);
    s.schedule(200, LatencyEventKind::FeedDeliver, 2);
    s.pop_next();
    EXPECT_EQ(s.now_ns(), 100);

    s.clear();
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.now_ns(), 0);

    // After clear, the seq counter is reset, so two ties at t=50 again pop
    // in submission order from the fresh side.
    s.schedule(50, LatencyEventKind::OrderLand, 99);
    s.schedule(50, LatencyEventKind::FillReport, 88);
    auto a = s.pop_next();
    auto b = s.pop_next();
    EXPECT_EQ(a.handle, 99u);
    EXPECT_EQ(b.handle, 88u);
}

TEST(LatencyScheduler, LargeRandomizedHeapStaysSorted) {
    // Push N events at random times, then pop all and check non-decreasing
    // time_ns. With the tiebreaker, ties resolve to submission order so the
    // sequence is fully deterministic given the input order.
    LatencyScheduler s(4096);
    std::mt19937_64 rng(0xBADF00DULL);
    std::uniform_int_distribution<std::int64_t> tdist(0, 10'000'000);

    constexpr int N = 4000;
    for (int i = 0; i < N; ++i) {
        s.schedule(tdist(rng), LatencyEventKind::FeedDeliver,
                   static_cast<std::uint64_t>(i));
    }
    EXPECT_EQ(s.size(), static_cast<std::size_t>(N));

    std::int64_t prev_t = -1;
    std::uint64_t prev_seq_at_t = 0;
    for (int i = 0; i < N; ++i) {
        LatencyEvent e = s.pop_next();
        EXPECT_GE(e.time_ns, prev_t);
        if (e.time_ns == prev_t) {
            EXPECT_GT(e.seq, prev_seq_at_t);
        }
        prev_t = e.time_ns;
        prev_seq_at_t = e.seq;
    }
    EXPECT_TRUE(s.empty());
}

// ---- StageSampler ----------------------------------------------------------

TEST(StageSampler, ZeroAlwaysReturnsZero) {
    StageSampler smp(StageLatencyConfig{LatencyDistribution::Zero, 0, 0});
    EXPECT_TRUE(smp.is_zero());
    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(smp.sample(rng), 0);
    }
}

TEST(StageSampler, ConstantAlwaysReturnsMean) {
    StageSampler smp(StageLatencyConfig{LatencyDistribution::Constant,
                                        12345, 0});
    EXPECT_FALSE(smp.is_zero());
    std::mt19937_64 rng(1);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(smp.sample(rng), 12345);
    }
}

TEST(StageSampler, ExponentialMeanWithinTolerance) {
    constexpr std::int64_t mean_ns = 10'000;  // 10 µs
    StageSampler smp(StageLatencyConfig{LatencyDistribution::Exponential,
                                        mean_ns, 0});
    std::mt19937_64 rng(42);

    constexpr int N = 200'000;
    double sum = 0.0;
    std::int64_t min_v = INT64_MAX, max_v = 0;
    for (int i = 0; i < N; ++i) {
        std::int64_t x = smp.sample(rng);
        sum += static_cast<double>(x);
        min_v = std::min(min_v, x);
        max_v = std::max(max_v, x);
    }
    const double observed_mean = sum / static_cast<double>(N);
    // SE of mean for exponential with N draws is mean/sqrt(N) ≈ 22.4 ns.
    // ±3% of 10 µs = 300 ns is well outside SE; this should hold robustly.
    EXPECT_NEAR(observed_mean, static_cast<double>(mean_ns),
                0.03 * static_cast<double>(mean_ns));
    EXPECT_GE(min_v, 0);
    EXPECT_LT(min_v, mean_ns);  // memoryless dist has lots of mass below mean
}

TEST(StageSampler, LogNormalMeanWithinTolerance) {
    constexpr std::int64_t mean_ns   = 50'000;
    constexpr std::int64_t stddev_ns = 25'000;  // CV = 0.5, fat-tailed
    StageSampler smp(StageLatencyConfig{LatencyDistribution::LogNormal,
                                        mean_ns, stddev_ns});
    std::mt19937_64 rng(7);

    constexpr int N = 200'000;
    double sum = 0.0;
    for (int i = 0; i < N; ++i) {
        sum += static_cast<double>(smp.sample(rng));
    }
    const double observed_mean = sum / static_cast<double>(N);
    // LogNormal sample mean converges slower; allow ±5%.
    EXPECT_NEAR(observed_mean, static_cast<double>(mean_ns),
                0.05 * static_cast<double>(mean_ns));
}

TEST(StageSampler, SeededDeterminism) {
    // Two samplers with the same config + same RNG seed produce the same
    // sequence — this is what `latency_seed` independence buys at the
    // simulator level.
    StageSampler smp_a(StageLatencyConfig{LatencyDistribution::LogNormal,
                                          10'000, 5'000});
    StageSampler smp_b(StageLatencyConfig{LatencyDistribution::LogNormal,
                                          10'000, 5'000});
    std::mt19937_64 ra(0xDEADBEEFULL);
    std::mt19937_64 rb(0xDEADBEEFULL);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(smp_a.sample(ra), smp_b.sample(rb));
    }
}

TEST(StageSampler, AllSamplesNonNegative) {
    // Cast from double to int64 is the only place a negative could leak in;
    // assert non-negative across all distribution kinds.
    std::mt19937_64 rng(99);
    for (auto kind : {LatencyDistribution::Constant,
                      LatencyDistribution::Exponential,
                      LatencyDistribution::LogNormal}) {
        StageSampler smp(StageLatencyConfig{kind, 1000, 500});
        for (int i = 0; i < 10'000; ++i) {
            EXPECT_GE(smp.sample(rng), 0);
        }
    }
}
