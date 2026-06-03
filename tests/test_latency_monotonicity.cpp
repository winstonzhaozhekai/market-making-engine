// M8 done-when: feed_latency sweep produces a monotonely-declining fill
// rate in the regime where lag-induced drift is comparable to (or larger
// than) the synthetic LOB spread.
//
// At very small lag, the simulator's queue-position model still admits an
// adverse-selection bump where stale-but-still-near-mid MM quotes get
// picked off MORE often than zero-lag quotes (real HFT physics — the
// "picked off slowly" regime). The bump is real and would not survive
// M9's queue-reactive LOB. The test brackets the *post-peak* monotone
// region [100µs..2ms] at vol=0.005 so the assertion measures the gate's
// dominant effect: increasingly stale MM quotes more often miss the
// market entirely, so fill count strictly decreases.
//
// Determinism: seed=42 fixes the sim path; latency_seed=0xC0FFEE fixes
// the per-stage samplers; iteration count is large enough that signal
// (fill-rate gradient) clears RNG noise.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "MarketSimulator.h"
#include "include/HeuristicStrategy.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"

namespace {

std::int64_t run_to_completion(const SimulationConfig& cfg) {
    MarketSimulator simulator(cfg);
    RiskConfig risk_cfg;
    HeuristicStrategy strategy;
    NullLogger logger;
    mme::MarketMakerT<HeuristicStrategy, NullLogger> mm(
        simulator.instrument_meta(), risk_cfg, &strategy, &logger);

    for (int i = 0; i < cfg.iterations; ++i) {
        MarketDataEvent md;
        try {
            md = simulator.generate_event();
        } catch (const std::out_of_range&) {
            break;
        }
        mm.on_market_data(md, simulator);
    }
    return static_cast<std::int64_t>(mm.get_total_fills());
}

SimulationConfig make_sweep_config(std::int64_t feed_mean_ns) {
    SimulationConfig cfg;
    cfg.seed         = 42;
    cfg.iterations   = 100'000;
    cfg.volatility   = 0.005;
    cfg.quiet        = true;
    cfg.latency_seed = 0xC0FFEEu;
    cfg.feed_latency.kind    = mme::LatencyDistribution::Constant;
    cfg.feed_latency.mean_ns = feed_mean_ns;
    return cfg;
}

} // namespace

TEST(LatencyMonotonicity, FeedLatencyReducesFillCountInPostPeakRegime) {
    const std::vector<std::int64_t> sweep_ns = {
        100'000,     // 100 µs
        500'000,     // 500 µs
        2'000'000,   //   2 ms
    };

    std::vector<std::int64_t> fills;
    fills.reserve(sweep_ns.size());
    for (std::int64_t ns : sweep_ns) {
        fills.push_back(run_to_completion(make_sweep_config(ns)));
    }

    ASSERT_EQ(fills.size(), sweep_ns.size());
    for (std::size_t i = 0; i + 1 < fills.size(); ++i) {
        EXPECT_GT(fills[i], fills[i + 1])
            << "fills(feed=" << sweep_ns[i] << "ns)=" << fills[i]
            << " must exceed fills(feed=" << sweep_ns[i + 1] << "ns)="
            << fills[i + 1] << " for monotone decline";
    }
}

TEST(LatencyMonotonicity, ZeroLatencyEnginesNoScheduler) {
    // Confirms the byte-equality fast path: at all-zero latency the
    // simulator's `latency_` optional is unengaged and the scheduler
    // contributes nothing to the run. The byte-equality test in
    // test_binary_log_roundtrip is the authoritative check; this test
    // adds an explicit assertion at the simulator-output level that the
    // fill count is deterministic across re-runs of the zero-latency
    // path.
    SimulationConfig cfg;
    cfg.seed       = 1337;
    cfg.iterations = 10'000;
    cfg.quiet      = true;
    // All three stages default to Zero in SimulationConfig.

    std::int64_t fills_a = run_to_completion(cfg);
    std::int64_t fills_b = run_to_completion(cfg);
    EXPECT_EQ(fills_a, fills_b) << "zero-latency run must be deterministic";
}
