// Isolated unit tests for the HLR queue-reactive LOB primitive
// (`include/QueueReactiveLob.h`). The primitive is a pure stochastic
// state machine emitting events — no engine, no Ticks, no chrono — so
// these tests exercise it without touching MarketSimulator or
// MatchingEngine. Wiring into the simulator + matching engine lands in
// M9/2.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "Order.h"
#include "include/QueueReactiveLob.h"

using mme::HLRConfig;
using mme::HLREvent;
using mme::HLREventKind;
using mme::HLRSide;

namespace {

constexpr std::int64_t kProductionStepNs = 1000;  // 1 µs, M8-matched

}  // namespace

TEST(QueueReactiveLob, ConstructInitializesState) {
    HLRConfig cfg;
    HLRSide bid(Side::BUY, cfg);
    for (int i = 0; i < cfg.num_levels; ++i) {
        EXPECT_EQ(bid.queue_at(i), cfg.initial_queue[i]);
    }
    EXPECT_EQ(bid.side(), Side::BUY);
    EXPECT_EQ(bid.num_levels(), cfg.num_levels);
}

TEST(QueueReactiveLob, EventsCarrySideTag) {
    HLRConfig cfg;
    HLRSide ask(Side::SELL, cfg);
    std::mt19937_64 rng(0x5A115);
    std::vector<HLREvent> events;

    // Drive long enough that at least one event of any kind has fired.
    for (int i = 0; i < 100'000 && events.empty(); ++i) {
        ask.step(kProductionStepNs, rng, events);
    }
    ASSERT_FALSE(events.empty());
    for (const auto& e : events) {
        EXPECT_EQ(e.side, Side::SELL);
    }
}

TEST(QueueReactiveLob, StepKeepsQueuesNonNegative) {
    HLRConfig cfg;
    HLRSide bid(Side::BUY, cfg);
    std::mt19937_64 rng(0xCAFEBABE);
    std::vector<HLREvent> events;
    for (int i = 0; i < 200'000; ++i) {
        events.clear();
        bid.step(kProductionStepNs, rng, events);
        for (int lvl = 0; lvl < cfg.num_levels; ++lvl) {
            ASSERT_GE(bid.queue_at(lvl), 0);
        }
    }
}

TEST(QueueReactiveLob, SameSeedReproducibleEventStream) {
    auto run = []() {
        HLRSide bid(Side::BUY, HLRConfig{});
        std::mt19937_64 rng(0xDEADBEEF);
        std::vector<HLREvent> all;
        std::vector<HLREvent> step_buf;
        for (int i = 0; i < 50'000; ++i) {
            step_buf.clear();
            bid.step(kProductionStepNs, rng, step_buf);
            for (const auto& e : step_buf) all.push_back(e);
        }
        return all;
    };
    const auto a = run();
    const auto b = run();
    ASSERT_EQ(a.size(), b.size());
    EXPECT_GT(a.size(), 0u);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(static_cast<int>(a[i].kind),
                  static_cast<int>(b[i].kind));
        EXPECT_EQ(a[i].level, b[i].level);
        EXPECT_EQ(a[i].qty,   b[i].qty);
        EXPECT_EQ(a[i].side,  b[i].side);
    }
}

TEST(QueueReactiveLob, DifferentSeedsDiverge) {
    auto run = [](std::uint64_t seed) {
        HLRSide bid(Side::BUY, HLRConfig{});
        std::mt19937_64 rng(seed);
        std::vector<HLREvent> all;
        std::vector<HLREvent> step_buf;
        for (int i = 0; i < 50'000; ++i) {
            step_buf.clear();
            bid.step(kProductionStepNs, rng, step_buf);
            for (const auto& e : step_buf) all.push_back(e);
        }
        return all;
    };
    const auto a = run(11);
    const auto b = run(22);
    ASSERT_GT(a.size(), 0u);
    ASSERT_GT(b.size(), 0u);
    // Trivially-equal head probability is vanishingly small at 50k steps.
    bool any_diff = a.size() != b.size();
    if (!any_diff) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (static_cast<int>(a[i].kind) != static_cast<int>(b[i].kind) ||
                a[i].level != b[i].level || a[i].qty != b[i].qty) {
                any_diff = true;
                break;
            }
        }
    }
    EXPECT_TRUE(any_diff);
}

TEST(QueueReactiveLob, ZeroQueueEmitsNoCancels) {
    HLRConfig cfg;
    cfg.initial_queue.fill(0);
    cfg.lambda_per_ns.fill(0.0);  // no arrivals to repopulate
    cfg.theta_per_ns = 0.0;
    // mu nonzero: with q=0 everywhere, no cancel event should fire.

    HLRSide bid(Side::BUY, cfg);
    std::mt19937_64 rng(99);
    std::vector<HLREvent> events;
    for (int i = 0; i < 200'000; ++i) {
        bid.step(kProductionStepNs, rng, events);
    }
    EXPECT_TRUE(events.empty());
    for (int i = 0; i < cfg.num_levels; ++i) {
        EXPECT_EQ(bid.queue_at(i), 0);
    }
}

TEST(QueueReactiveLob, MarketOrderRequiresLevelZeroQty) {
    // Disable arrivals + cancels; only theta active. Start with q_0=0.
    // No MarketOrder events should fire (gated on q_[0] > 0).
    HLRConfig cfg;
    cfg.initial_queue.fill(0);
    cfg.lambda_per_ns.fill(0.0);
    cfg.mu_per_unit_per_ns = 0.0;
    cfg.theta_per_ns = 1.0e-6;  // 1000/s — plenty if it were going to fire

    HLRSide bid(Side::BUY, cfg);
    std::mt19937_64 rng(7);
    std::vector<HLREvent> events;
    for (int i = 0; i < 200'000; ++i) {
        bid.step(kProductionStepNs, rng, events);
    }
    EXPECT_TRUE(events.empty());
}

TEST(QueueReactiveLob, MarketOrderDecrementsLevelZeroWhenItFires) {
    // Disable arrivals + cancels; only theta. Start with q_0=200.
    // After enough steps, q_0 must have strictly decreased.
    HLRConfig cfg;
    cfg.initial_queue.fill(0);
    cfg.initial_queue[0] = 200;
    cfg.lambda_per_ns.fill(0.0);
    cfg.mu_per_unit_per_ns = 0.0;
    cfg.theta_per_ns = 1.0e-6;
    cfg.mean_market_size = 5;

    HLRSide bid(Side::BUY, cfg);
    std::mt19937_64 rng(7);
    std::vector<HLREvent> events;
    for (int i = 0; i < 200'000; ++i) {
        events.clear();
        bid.step(kProductionStepNs, rng, events);
    }
    EXPECT_LT(bid.queue_at(0), 200);
    EXPECT_GE(bid.queue_at(0), 0);
}

TEST(QueueReactiveLob, LongRunQueueIsBoundedAndNonZero) {
    // Loose sanity for the default config: across many steps the mean q_0
    // is bounded away from 0 (arrivals dominate) and below some sane cap
    // (cancels keep it finite). Exact calibration belongs in M9/4 against
    // the realism tests; this is just "primitive doesn't degenerate".
    HLRConfig cfg;
    HLRSide bid(Side::BUY, cfg);
    std::mt19937_64 rng(0xC0FFEE);
    std::vector<HLREvent> events;

    // Warmup.
    for (int i = 0; i < 200'000; ++i) {
        events.clear();
        bid.step(kProductionStepNs, rng, events);
    }

    // Sample window.
    double sum = 0.0;
    constexpr int kSamples = 200'000;
    int min_q = bid.queue_at(0);
    int max_q = bid.queue_at(0);
    for (int i = 0; i < kSamples; ++i) {
        events.clear();
        bid.step(kProductionStepNs, rng, events);
        const int q0 = bid.queue_at(0);
        sum += q0;
        min_q = std::min(min_q, q0);
        max_q = std::max(max_q, q0);
    }
    const double mean = sum / kSamples;
    EXPECT_GT(mean, 1.0)   << "queue collapsed near zero, min=" << min_q;
    EXPECT_LT(mean, 500.0) << "queue diverged, max=" << max_q;
}
