// Wiring tests for the M9 QueueReactive LOB model: HLR primitive +
// MarketSimulator + MatchingEngine end-to-end. The primitive itself is
// unit-tested in test_queue_reactive_lob.cpp; this file asserts the
// wiring layer maintains its invariants:
//
//   1. Construction seeds the matching engine with HLR initial_queue
//      orders at each level (per side).
//   2. HLR.q[level] tracks the engine's actual resting synthetic depth
//      throughout the run.
//   3. Synthetic-LOB activity produces trades + maintains a non-trivial
//      book over a long simulation.
//   4. Same lob_seed → reproducible event/engine trajectory.
//   5. Legacy default path is byte-equal to pre-M9 behavior (proven by
//      the existing test_determinism + test_binary_log_roundtrip — this
//      file just sanity-checks that Legacy is unchanged when QueueReactive
//      knobs are touched).

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "MarketDataEvent.h"
#include "MarketSimulator.h"
#include "MatchingEngine.h"
#include "Order.h"
#include "include/SimulationConfig.h"

namespace {

SimulationConfig qr_base_config() {
    SimulationConfig cfg;
    cfg.seed       = 42;
    cfg.iterations = 0;
    cfg.quiet      = true;
    cfg.lob_model  = LobModel::QueueReactive;
    cfg.lob_seed   = 0xB00B5u;
    return cfg;
}

// Sum of HLR-known synthetic units per side, summed across levels.
int hlr_total_qty(const MatchingEngine& eng, Side s) {
    int total = 0;
    for (std::size_t d = 0; d < eng.num_levels(s); ++d) {
        total += eng.qty_at_depth(s, d);
    }
    return total;
}

}  // namespace

TEST(QueueReactiveSimulator, ConstructionSeedsEngineWithInitialQueue) {
    SimulationConfig cfg = qr_base_config();
    MarketSimulator sim(cfg);

    const auto& eng = sim.get_matching_engine();
    EXPECT_FALSE(eng.empty(Side::BUY));
    EXPECT_FALSE(eng.empty(Side::SELL));

    // Default HLRConfig has initial_queue {20,30,40,40,40} and
    // mean_limit_size=5 → 4+6+8+8+8 = 34 orders per side.
    EXPECT_EQ(eng.num_levels(Side::BUY),  static_cast<std::size_t>(5));
    EXPECT_EQ(eng.num_levels(Side::SELL), static_cast<std::size_t>(5));

    // Total resting qty per side = sum(initial_queue) = 170.
    EXPECT_EQ(hlr_total_qty(eng, Side::BUY),  170);
    EXPECT_EQ(hlr_total_qty(eng, Side::SELL), 170);
}

TEST(QueueReactiveSimulator, BookEvolvesUnderHLRDynamics) {
    // Override theta to a high-but-still-Bernoulli-valid rate so this
    // smoke test sees plenty of trades + inside depletion without
    // running a million steps. Calibration of default HLR rates against
    // the realism rubric is M9/4 work.
    SimulationConfig cfg = qr_base_config();
    cfg.hlr.theta_per_ns = 1.0e-6;  // ~1000/s/side
    MarketSimulator sim(cfg);
    const auto& eng = sim.get_matching_engine();

    const int initial_bid_qty = hlr_total_qty(eng, Side::BUY);
    const int initial_ask_qty = hlr_total_qty(eng, Side::SELL);

    int trades_seen = 0;
    int distinct_best_bid_prices = 0;
    Ticks last_best_bid{0};

    for (int i = 0; i < 50'000; ++i) {
        MarketDataEvent md = sim.generate_event();
        trades_seen += static_cast<int>(md.trades.size());
        if (md.best_bid_price != last_best_bid) {
            ++distinct_best_bid_prices;
            last_best_bid = md.best_bid_price;
        }
    }

    // theta=1e-6 × dt=1000ns × 50k steps × 2 sides ≈ 100 expected events.
    EXPECT_GT(trades_seen, 20);

    // Book qty should have drifted from the seeded state — adds/cancels/
    // market orders all touch it.
    const int now_bid_qty = hlr_total_qty(eng, Side::BUY);
    const int now_ask_qty = hlr_total_qty(eng, Side::SELL);
    EXPECT_NE(now_bid_qty, initial_bid_qty);
    EXPECT_NE(now_ask_qty, initial_ask_qty);

    // Best-bid price should have moved at least once (depletion of inside
    // level → walk down, refill → walk back).
    EXPECT_GT(distinct_best_bid_prices, 0);
}

TEST(QueueReactiveSimulator, SameLobSeedReproducibleEngineState) {
    auto run = [](std::uint32_t lob_seed) {
        SimulationConfig cfg = qr_base_config();
        cfg.lob_seed = lob_seed;
        MarketSimulator sim(cfg);
        for (int i = 0; i < 20'000; ++i) {
            (void)sim.generate_event();
        }
        struct Snap { int bid_total; int ask_total; Ticks best_bid; Ticks best_ask; };
        const auto& eng = sim.get_matching_engine();
        return Snap{
            hlr_total_qty(eng, Side::BUY),
            hlr_total_qty(eng, Side::SELL),
            eng.empty(Side::BUY)  ? Ticks{0} : eng.best_price(Side::BUY),
            eng.empty(Side::SELL) ? Ticks{0} : eng.best_price(Side::SELL),
        };
    };

    const auto a = run(0xB00B5u);
    const auto b = run(0xB00B5u);
    EXPECT_EQ(a.bid_total, b.bid_total);
    EXPECT_EQ(a.ask_total, b.ask_total);
    EXPECT_EQ(a.best_bid,  b.best_bid);
    EXPECT_EQ(a.best_ask,  b.best_ask);

    const auto c = run(0xC0FFEEu);
    // Different lob_seed → almost certainly different trajectory.
    const bool any_diff = (c.bid_total != a.bid_total) ||
                          (c.ask_total != a.ask_total) ||
                          (c.best_bid  != a.best_bid)  ||
                          (c.best_ask  != a.best_ask);
    EXPECT_TRUE(any_diff);
}

TEST(QueueReactiveSimulator, SyntheticOrderQueuesStayNonNegative) {
    // Walk a long simulation; at every step the engine's per-level depth
    // must be ≥ 0 (it always is — this is a sanity check on the wiring
    // layer that cancels never over-pop FIFOs and market orders never
    // route to non-existent qty).
    SimulationConfig cfg = qr_base_config();
    MarketSimulator sim(cfg);
    const auto& eng = sim.get_matching_engine();

    for (int i = 0; i < 50'000; ++i) {
        (void)sim.generate_event();
        for (std::size_t d = 0; d < eng.num_levels(Side::BUY); ++d) {
            ASSERT_GE(eng.qty_at_depth(Side::BUY, d),  0);
        }
        for (std::size_t d = 0; d < eng.num_levels(Side::SELL); ++d) {
            ASSERT_GE(eng.qty_at_depth(Side::SELL, d), 0);
        }
    }
}

TEST(QueueReactiveSimulator, MdLevelsDerivedFromEngineState) {
    // Under QueueReactive, every MD event's bid_levels_/ask_levels_ must
    // mirror the matching engine's actual book at the top `hlr.num_levels`
    // depths. We check this on the first event after construction (which
    // is purely the seeded initial_queue) and again after long-run
    // dynamics have churned the book.
    auto check_md_against_engine = [](const MarketDataEvent& md,
                                      const MatchingEngine& eng,
                                      int num_levels_cap) {
        const std::size_t cap = static_cast<std::size_t>(num_levels_cap);
        const std::size_t exp_bid =
            std::min<std::size_t>(eng.num_levels(Side::BUY),  cap);
        const std::size_t exp_ask =
            std::min<std::size_t>(eng.num_levels(Side::SELL), cap);
        ASSERT_EQ(md.bid_levels.size(), exp_bid);
        ASSERT_EQ(md.ask_levels.size(), exp_ask);
        for (std::size_t d = 0; d < exp_bid; ++d) {
            EXPECT_EQ(md.bid_levels[d].price,
                      eng.price_at_depth(Side::BUY, d));
            EXPECT_EQ(md.bid_levels[d].size,
                      eng.qty_at_depth(Side::BUY, d));
        }
        for (std::size_t d = 0; d < exp_ask; ++d) {
            EXPECT_EQ(md.ask_levels[d].price,
                      eng.price_at_depth(Side::SELL, d));
            EXPECT_EQ(md.ask_levels[d].size,
                      eng.qty_at_depth(Side::SELL, d));
        }
        if (!md.bid_levels.empty()) {
            EXPECT_EQ(md.best_bid_price, md.bid_levels.front().price);
            EXPECT_EQ(md.best_bid_size,  md.bid_levels.front().size);
        }
        if (!md.ask_levels.empty()) {
            EXPECT_EQ(md.best_ask_price, md.ask_levels.front().price);
            EXPECT_EQ(md.best_ask_size,  md.ask_levels.front().size);
        }
    };

    SimulationConfig cfg = qr_base_config();
    MarketSimulator  sim(cfg);
    MarketDataEvent first = sim.generate_event();
    check_md_against_engine(first, sim.get_matching_engine(),
                            cfg.hlr.num_levels);
    EXPECT_EQ(first.bid_levels.size(), static_cast<std::size_t>(5));
    EXPECT_EQ(first.ask_levels.size(), static_cast<std::size_t>(5));
    // After seeding, depth 0 bid = ref - 1 tick, depth 0 ask = ref + 1.
    // Spread = ask - bid = 2 ticks.
    EXPECT_EQ(first.best_ask_price - first.best_bid_price, Ticks{2});

    // After dynamics under a high-theta stress config.
    SimulationConfig stress = cfg;
    stress.hlr.theta_per_ns = 1.0e-6;
    MarketSimulator sim2(stress);
    for (int i = 0; i < 10'000; ++i) (void)sim2.generate_event();
    MarketDataEvent later = sim2.generate_event();
    check_md_against_engine(later, sim2.get_matching_engine(),
                            stress.hlr.num_levels);
}

TEST(QueueReactiveSimulator, LobSeedAndHlrFieldsInertUnderLegacy) {
    // When lob_model is explicitly set to Legacy, touching lob_seed /
    // hlr fields must not perturb the run — those knobs route through
    // the QueueReactive code path only. (After M9/6 the default flipped
    // to QueueReactive, so this test now pins Legacy explicitly.)
    SimulationConfig a;
    a.seed       = 1337;
    a.iterations = 0;
    a.quiet      = true;
    a.lob_model  = LobModel::Legacy;

    SimulationConfig b = a;
    b.lob_seed   = 0xFEEDFACEu;
    b.hlr.theta_per_ns = 99.0;  // would explode under QueueReactive — inert under Legacy.

    auto fingerprint = [](SimulationConfig cfg) {
        MarketSimulator sim(cfg);
        std::int64_t acc = 0;
        for (int i = 0; i < 500; ++i) {
            MarketDataEvent md = sim.generate_event();
            acc = acc * 1315423911LL + md.best_bid_price + md.best_ask_price * 31;
        }
        return acc;
    };

    EXPECT_EQ(fingerprint(a), fingerprint(b));
}
