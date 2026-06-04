// M9/4 realism tests for the QueueReactive LOB. These assert the
// "done-when" properties from the ROADMAP M9 entry:
//
//   1. Realistic spread distribution — varies, not stuck at the seeded
//      2-tick floor.
//   2. Realistic top-of-book queue depth — non-trivial distribution,
//      not the Legacy decoration's 1..10 uniform.
//   3. MM fill rate sensitive to quote price relative to top-of-book —
//      under QueueReactive, the M8 adverse-selection bump disappears
//      because synthetic makers at the inside (with time priority)
//      absorb the aggressor flow before MM stale quotes can be hit.
//      Concrete check: fill_count(feed_latency=0) >= fill_count(feed=50µs)
//      with no pre-peak spike. The full sweep monotone-decreases (GE).
//
// The shared `realism_config()` calibrates HLR rates for a deep-book,
// high-aggressor-flow regime that gives the realism + sensitivity tests
// a clean signal in 100k iterations. Default HLR rates from
// `include/QueueReactiveLob.h` are deliberately quieter; tuning the
// defaults against this rubric is M9/6's job after the test bar is set.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

#include "MarketSimulator.h"
#include "include/HeuristicStrategy.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"

namespace {

SimulationConfig realism_config(std::int64_t feed_mean_ns = 0) {
    SimulationConfig cfg;
    cfg.seed       = 42;
    cfg.lob_seed   = 0xB00B5u;
    cfg.iterations = 100'000;
    cfg.quiet      = true;
    cfg.lob_model  = LobModel::QueueReactive;
    // Calibrated for deep book + high aggressor flow.
    //   lambda_0 = 5e-5 /ns × 5 units = 250 units/µs arrivals at L0
    //   mu       = 1e-7 /unit/ns      → cancel rate q*5e-7 units/ns/unit
    //   theta    = 1e-5 /ns × 5 units = 50 units/µs aggressor consumption
    //   q*_0     = (lambda - theta) × mean / mu  ≈ 400 units
    //   ~1000 aggressor events / 100k iterations.
    cfg.hlr.lambda_per_ns      = {{5e-5, 2.5e-5, 1.7e-5, 1.2e-5, 1.0e-5}};
    cfg.hlr.mu_per_unit_per_ns = 1.0e-7;
    cfg.hlr.theta_per_ns       = 1.0e-5;
    cfg.hlr.initial_queue      = {{400, 400, 400, 400, 400}};
    cfg.latency_seed           = 0xC0FFEEu;
    cfg.feed_latency.kind      = mme::LatencyDistribution::Constant;
    cfg.feed_latency.mean_ns   = feed_mean_ns;
    return cfg;
}

double mean(const std::vector<int>& xs) {
    return std::accumulate(xs.begin(), xs.end(), 0.0) / xs.size();
}
double stddev(const std::vector<int>& xs, double m) {
    double sq = 0.0;
    for (int x : xs) sq += (x - m) * (x - m);
    return std::sqrt(sq / xs.size());
}

std::int64_t run_mm_capture_fills(const SimulationConfig& cfg) {
    MarketSimulator sim(cfg);
    HeuristicStrategy strategy;
    NullLogger logger;
    mme::MarketMakerT<HeuristicStrategy, NullLogger> mm(
        sim.instrument_meta(), RiskConfig{}, &strategy, &logger);
    for (int i = 0; i < cfg.iterations; ++i) {
        MarketDataEvent md;
        try { md = sim.generate_event(); }
        catch (const std::out_of_range&) { break; }
        mm.on_market_data(md, sim);
    }
    return static_cast<std::int64_t>(mm.get_total_fills());
}

}  // namespace

TEST(LobRealism, SpreadDistributionVariesAroundMinTick) {
    // Tighter equilibrium for this test: lower lambda + higher mu so q*
    // sits in the 40-80 range and theta depletes level 0 occasionally,
    // walking the inside down by ≥1 tick and widening spread. The
    // headline fill-rate test uses a deeper-book config — different
    // realism axes warrant different calibrations rather than one
    // setting trying to satisfy both.
    SimulationConfig cfg = realism_config();
    cfg.iterations = 50'000;
    cfg.hlr.lambda_per_ns      = {{1.5e-5, 1.0e-5, 7e-6, 5e-6, 4e-6}};
    cfg.hlr.mu_per_unit_per_ns = 5.0e-7;
    cfg.hlr.theta_per_ns       = 1.0e-5;
    cfg.hlr.initial_queue      = {{50, 60, 70, 70, 70}};

    MarketSimulator sim(cfg);
    std::vector<int> spreads;
    spreads.reserve(cfg.iterations);
    for (int i = 0; i < cfg.iterations; ++i) {
        auto md = sim.generate_event();
        if (md.best_bid_price > Ticks{0} && md.best_ask_price > Ticks{0}) {
            spreads.push_back(static_cast<int>(md.best_ask_price - md.best_bid_price));
        }
    }
    ASSERT_FALSE(spreads.empty());

    const int min_s = *std::min_element(spreads.begin(), spreads.end());
    const int max_s = *std::max_element(spreads.begin(), spreads.end());
    const double m  = mean(spreads);

    // Synthetic top sits at ref±1 → min spread = 2 ticks when both sides
    // have inside qty.
    EXPECT_EQ(min_s, 2);
    // Inside depletion + walk-down produces wider spreads sometimes.
    EXPECT_GT(max_s, 2)
        << "spread never widens — calibration too deep, level 0 never depletes";
    EXPECT_GE(m, 2.0);
    EXPECT_LT(m, 10.0);
}

TEST(LobRealism, TopOfBookQueueDepthVariesNotDecoration) {
    SimulationConfig cfg = realism_config();
    cfg.iterations = 50'000;
    MarketSimulator sim(cfg);

    std::vector<int> bid_sizes;
    bid_sizes.reserve(cfg.iterations);
    for (int i = 0; i < cfg.iterations; ++i) {
        auto md = sim.generate_event();
        bid_sizes.push_back(md.best_bid_size);
    }

    const double m = mean(bid_sizes);
    const double s = stddev(bid_sizes, m);

    // Legacy decoration produced uniform 1..10 → mean ≈ 5, stddev ≈ 2.6.
    // QueueReactive at the calibrated config should sit much deeper with
    // wider fluctuations driven by birth-death dynamics.
    EXPECT_GT(m, 20.0)
        << "top-of-book mean depth=" << m
        << " — too shallow vs Legacy decoration's 1..10 uniform";
    EXPECT_GT(s, 5.0)
        << "top-of-book depth stddev=" << s
        << " — not enough variability vs Legacy decoration's ~2.6";
}

TEST(LobRealism, MmFillRateMonotoneFromZeroLatency) {
    // Headline M9 done-when: under QueueReactive, MM fill count must
    // not exhibit the M8 small-latency adverse-selection bump. Sweep
    // feed_latency starting from 0 and assert GE chain. Strict GT is
    // not asserted because plateau regions are legitimate (constant mid
    // under QueueReactive means MM's stale quote often equals its fresh
    // quote, so latency contributes nothing in some regimes).
    const std::vector<std::int64_t> sweep_ns = {
        0,
        50'000,     //  50 µs
        100'000,    // 100 µs
        500'000,    // 500 µs
        2'000'000,  //   2 ms
    };

    std::vector<std::int64_t> fills;
    fills.reserve(sweep_ns.size());
    for (auto ns : sweep_ns) {
        fills.push_back(run_mm_capture_fills(realism_config(ns)));
    }

    // Non-degenerate signal at zero latency.
    EXPECT_GT(fills.front(), 0)
        << "MM never filled at feed=0 — calibration or strategy issue";

    // GE chain across the full sweep.
    for (std::size_t i = 0; i + 1 < fills.size(); ++i) {
        EXPECT_GE(fills[i], fills[i + 1])
            << "fills(feed=" << sweep_ns[i] << "ns)=" << fills[i]
            << " must be >= fills(feed=" << sweep_ns[i + 1] << "ns)="
            << fills[i + 1]
            << " — QueueReactive should suppress the adverse-selection bump";
    }
}

TEST(LobRealism, MmFillRateDeterministicAtZeroLatency) {
    // Sanity: at zero latency under QueueReactive, the all-zero fast
    // path is engaged (latency_ stays unengaged), and same seed + same
    // lob_seed reproduces the exact fill count.
    SimulationConfig cfg = realism_config(0);
    const auto a = run_mm_capture_fills(cfg);
    const auto b = run_mm_capture_fills(cfg);
    EXPECT_EQ(a, b);
}
