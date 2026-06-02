#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <vector>
#include "include/Instrument.h"
#include "include/Strategy.h"
#include "include/RollingEstimators.h"
#include "include/HeuristicStrategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"

namespace {

constexpr double EPS = 1e-6;
const Instrument kIns{0.01};
Ticks T(double dollars) { return kIns.to_ticks(dollars); }
double D(Ticks t) { return kIns.to_price(t); }

using time_point = std::chrono::system_clock::time_point;

time_point base_time() {
    return std::chrono::system_clock::from_time_t(1000000);
}

// Bundle owns the backing vectors so the snapshot's spans stay valid for the
// strategy call. view() rebinds spans against the (possibly grown) buffers.
struct SnapBundle {
    std::vector<OrderLevel> bid_levels;
    std::vector<OrderLevel> ask_levels;
    std::vector<Trade>      trades;
    StrategySnapshot        snap;

    const StrategySnapshot& view() {
        snap.bid_levels = bid_levels;
        snap.ask_levels = ask_levels;
        snap.trades     = trades;
        return snap;
    }
};

SnapBundle make_snap(double mid, int position = 0, int max_pos = 1000) {
    SnapBundle b;
    b.snap.best_bid = T(mid - 0.05);
    b.snap.best_ask = T(mid + 0.05);
    b.snap.mid_price = mid;
    b.bid_levels.emplace_back(T(mid - 0.05), 100, 1ULL, base_time());
    b.ask_levels.emplace_back(T(mid + 0.05), 100, 2ULL, base_time());
    b.snap.position = position;
    b.snap.max_position = max_pos;
    b.snap.tick_size = kIns.tick_size;
    b.snap.timestamp = base_time();
    b.snap.sequence_number = 1;
    return b;
}

Trade make_trade(Side side, double price, int size) {
    Trade t;
    t.aggressor_side = side;
    t.price = T(price);
    t.size = size;
    t.trade_id = 100;
    t.timestamp = base_time();
    return t;
}

// ============================================================
// RollingVolatility tests (3)
// ============================================================

TEST(StrategyBehavior, vol_zero_with_less_than_two_points) {
    RollingVolatility vol(100);
    EXPECT_EQ(vol.sigma(), 0.0);
    vol.on_mid(100.0);
    EXPECT_EQ(vol.sigma(), 0.0);
}

TEST(StrategyBehavior, vol_zero_for_constant_prices) {
    RollingVolatility vol(100);
    for (int i = 0; i < 10; ++i) vol.on_mid(100.0);
    EXPECT_EQ(vol.sigma(), 0.0);
}

TEST(StrategyBehavior, vol_known_value) {
    RollingVolatility vol(100);
    vol.on_mid(100.0);
    vol.on_mid(101.0);
    vol.on_mid(100.0);
    vol.on_mid(101.0);
    vol.on_mid(100.0);
    double s = vol.sigma();
    EXPECT_GT(s, 0.005);
    EXPECT_LT(s, 0.015);
}

// ============================================================
// RollingOFI tests (3)
// ============================================================

TEST(StrategyBehavior, ofi_zero_with_no_trades) {
    RollingOFI ofi(50);
    EXPECT_EQ(ofi.normalized_ofi(), 0.0);
}

TEST(StrategyBehavior, ofi_plus_one_for_all_buys) {
    RollingOFI ofi(50);
    std::vector<Trade> trades;
    trades.push_back(make_trade(Side::BUY, 100.0, 10));
    trades.push_back(make_trade(Side::BUY, 100.0, 20));
    ofi.on_trades(trades);
    EXPECT_NEAR(ofi.normalized_ofi(), 1.0, EPS);
}

TEST(StrategyBehavior, ofi_mixed_trades) {
    RollingOFI ofi(50);
    std::vector<Trade> trades;
    trades.push_back(make_trade(Side::BUY, 100.0, 30));
    trades.push_back(make_trade(Side::SELL, 100.0, 10));
    ofi.on_trades(trades);
    EXPECT_NEAR(ofi.normalized_ofi(), 0.5, EPS);
}

// ============================================================
// HeuristicStrategy tests (2)
// ============================================================

TEST(StrategyBehavior, heuristic_output_matches_old_logic) {
    HeuristicStrategy strat;
    auto snap = make_snap(100.0, 0, 1000);

    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_NEAR(D(d.bid_price), 100.0 - 0.01, 1e-4);
    EXPECT_NEAR(D(d.ask_price), 100.0 + 0.01, 1e-4);
    EXPECT_TRUE(d.should_quote);
}

TEST(StrategyBehavior, heuristic_skew_direction) {
    // skew_factor=0.001 with |position|=15 gives a skew of ±0.015 (clamped to
    // ±0.01) — more than one full tick away from the flat-position quote, so
    // the direction survives the tick-grid snap.
    HeuristicStrategy strat;
    auto snap_long = make_snap(100.0, 15, 1000);
    QuoteDecision d_long = strat.compute_quotes(snap_long.view());
    EXPECT_LT(D(d_long.bid_price), 100.0 - 0.01);
    EXPECT_LT(D(d_long.ask_price), 100.0 + 0.01);

    auto snap_short = make_snap(100.0, -15, 1000);
    QuoteDecision d_short = strat.compute_quotes(snap_short.view());
    EXPECT_GT(D(d_short.bid_price), 100.0 - 0.01);
    EXPECT_GT(D(d_short.ask_price), 100.0 + 0.01);
}

// ============================================================
// A-S core tests (6)
// ============================================================

TEST(StrategyBehavior, as_determinism) {
    AvellanedaStoikovConfig cfg;
    AvellanedaStoikovStrategy s1(cfg);
    AvellanedaStoikovStrategy s2(cfg);

    auto snap = make_snap(100.0, 0, 1000);
    for (int i = 0; i < 5; ++i) {
        snap.snap.mid_price = 100.0 + i * 0.01;
        snap.snap.best_bid = T(snap.snap.mid_price - 0.05);
        snap.snap.best_ask = T(snap.snap.mid_price + 0.05);
    }
    QuoteDecision d1 = s1.compute_quotes(snap.view());
    QuoteDecision d2 = s2.compute_quotes(snap.view());
    EXPECT_EQ(d1.bid_price, d2.bid_price);
    EXPECT_EQ(d1.ask_price, d2.ask_price);
    EXPECT_EQ(d1.bid_size, d2.bid_size);
    EXPECT_EQ(d1.ask_size, d2.ask_size);
}

TEST(StrategyBehavior, as_reservation_shifts_down_when_long) {
    // Default gamma is small enough that q*gamma*sigma^2*T sits below one tick
    // for moderate positions and the snap erases the direction. Bump gamma so
    // the reservation shift comfortably clears the tick grid.
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.gamma = 10000.0;
    AvellanedaStoikovStrategy s_flat(cfg);
    AvellanedaStoikovStrategy s_long(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap_f = make_snap(mid, 0, 1000);
        auto snap_l = make_snap(mid, 50, 1000);
        s_flat.compute_quotes(snap_f.view());
        s_long.compute_quotes(snap_l.view());
    }

    auto snap_f = make_snap(100.0, 0, 1000);
    auto snap_l = make_snap(100.0, 50, 1000);
    QuoteDecision d_flat = s_flat.compute_quotes(snap_f.view());
    QuoteDecision d_long = s_long.compute_quotes(snap_l.view());

    double mid_flat = (D(d_flat.bid_price) + D(d_flat.ask_price)) / 2.0;
    double mid_long = (D(d_long.bid_price) + D(d_long.ask_price)) / 2.0;
    EXPECT_LT(mid_long, mid_flat);
}

TEST(StrategyBehavior, as_reservation_shifts_up_when_short) {
    // See note in as_reservation_shifts_down_when_long — bump gamma so the
    // reservation shift exceeds tick granularity.
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.gamma = 10000.0;
    AvellanedaStoikovStrategy s_flat(cfg);
    AvellanedaStoikovStrategy s_short(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap_f = make_snap(mid, 0, 1000);
        auto snap_s = make_snap(mid, -50, 1000);
        s_flat.compute_quotes(snap_f.view());
        s_short.compute_quotes(snap_s.view());
    }

    auto snap_f = make_snap(100.0, 0, 1000);
    auto snap_s = make_snap(100.0, -50, 1000);
    QuoteDecision d_flat = s_flat.compute_quotes(snap_f.view());
    QuoteDecision d_short = s_short.compute_quotes(snap_s.view());

    double mid_flat = (D(d_flat.bid_price) + D(d_flat.ask_price)) / 2.0;
    double mid_short = (D(d_short.bid_price) + D(d_short.ask_price)) / 2.0;
    EXPECT_GT(mid_short, mid_flat);
}

TEST(StrategyBehavior, as_spread_widens_with_high_vol) {
    AvellanedaStoikovConfig cfg;
    cfg.gamma = 50.0;
    cfg.vol_window = 5;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 50000.0;
    AvellanedaStoikovStrategy s_low(cfg);
    AvellanedaStoikovStrategy s_high(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        s_low.compute_quotes(snap.view());
    }

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? -2.0 : 2.0);
        auto snap = make_snap(mid, 0, 1000);
        s_high.compute_quotes(snap.view());
    }

    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d_low = s_low.compute_quotes(snap.view());
    QuoteDecision d_high = s_high.compute_quotes(snap.view());

    double spread_low = D(d_low.ask_price - d_low.bid_price);
    double spread_high = D(d_high.ask_price - d_high.bid_price);
    EXPECT_GT(spread_high, spread_low);
}

TEST(StrategyBehavior, as_spread_tightens_with_low_vol) {
    AvellanedaStoikovConfig cfg;
    cfg.gamma = 100.0;
    cfg.vol_window = 5;
    cfg.min_spread_bps = 200.0;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        strat.compute_quotes(snap.view());
    }
    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d = strat.compute_quotes(snap.view());
    double spread = D(d.ask_price - d.bid_price);
    double min_spread = 200.0 * 100.0 / 10000.0;
    EXPECT_NEAR(spread, min_spread, 0.01);
}

TEST(StrategyBehavior, as_min_floor_enforced) {
    AvellanedaStoikovConfig cfg;
    cfg.min_spread_bps = 50.0;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        strat.compute_quotes(snap.view());
    }
    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d = strat.compute_quotes(snap.view());
    double spread = D(d.ask_price - d.bid_price);
    double min_spread = 50.0 * 100.0 / 10000.0;
    EXPECT_GE(spread, min_spread - EPS);
}

// ============================================================
// Inventory skew tests (3)
// ============================================================

TEST(StrategyBehavior, as_long_ask_tighter) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap = make_snap(mid, 50, 1000);
        strat.compute_quotes(snap.view());
    }

    auto snap = make_snap(100.0, 50, 1000);
    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_GT(d.ask_size, d.bid_size);
}

TEST(StrategyBehavior, as_short_bid_tighter) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap = make_snap(mid, -50, 1000);
        strat.compute_quotes(snap.view());
    }

    auto snap = make_snap(100.0, -50, 1000);
    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_GT(d.bid_size, d.ask_size);
}

TEST(StrategyBehavior, as_max_inventory_max_asymmetry) {
    AvellanedaStoikovConfig cfg;
    cfg.base_size = 10;
    cfg.size_inventory_scale = 1.0;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 1000, 1000);
        strat.compute_quotes(snap.view());
    }
    auto snap = make_snap(100.0, 1000, 1000);
    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_EQ(d.bid_size, 1);
    EXPECT_EQ(d.ask_size, 20);
}

// ============================================================
// Adverse selection tests (3)
// ============================================================

TEST(StrategyBehavior, as_high_ofi_widens_spread) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 10;
    cfg.ofi_spread_factor = 1.0;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 5000.0;
    AvellanedaStoikovStrategy s_no_ofi(cfg);
    AvellanedaStoikovStrategy s_ofi(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.5);
        auto snap_no = make_snap(mid, 0, 1000);
        auto snap_ofi = make_snap(mid, 0, 1000);
        snap_ofi.trades.push_back(make_trade(Side::BUY, mid, 100));
        s_no_ofi.compute_quotes(snap_no.view());
        s_ofi.compute_quotes(snap_ofi.view());
    }

    auto snap_no = make_snap(100.0, 0, 1000);
    auto snap_ofi = make_snap(100.0, 0, 1000);
    snap_ofi.trades.push_back(make_trade(Side::BUY, 100.0, 100));
    QuoteDecision d_no = s_no_ofi.compute_quotes(snap_no.view());
    QuoteDecision d_ofi = s_ofi.compute_quotes(snap_ofi.view());

    double spread_no = D(d_no.ask_price - d_no.bid_price);
    double spread_ofi = D(d_ofi.ask_price - d_ofi.bid_price);
    EXPECT_GT(spread_ofi, spread_no);
}

TEST(StrategyBehavior, as_pull_on_toxic_true) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 5;
    cfg.toxic_ofi_threshold = 0.5;
    cfg.pull_on_toxic = true;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 0, 1000);
        snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
        strat.compute_quotes(snap.view());
    }

    auto snap = make_snap(100.0, 0, 1000);
    snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_FALSE(d.should_quote);
}

TEST(StrategyBehavior, as_pull_on_toxic_false_still_quotes_wider) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 5;
    cfg.toxic_ofi_threshold = 0.5;
    cfg.pull_on_toxic = false;
    cfg.ofi_spread_factor = 1.0;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 5000.0;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 0, 1000);
        snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
        strat.compute_quotes(snap.view());
    }

    auto snap = make_snap(100.0, 0, 1000);
    snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
    QuoteDecision d = strat.compute_quotes(snap.view());
    EXPECT_TRUE(d.should_quote);
}

// ============================================================
// Integration test (1)
// ============================================================

TEST(StrategyBehavior, integration_200_snapshots) {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 20;
    cfg.ofi_window = 10;
    cfg.base_size = 5;
    AvellanedaStoikovStrategy strat(cfg);

    QuoteDecision last;
    for (int i = 0; i < 200; ++i) {
        double mid = 100.0 + 0.5 * std::sin(i * 0.1);
        auto snap = make_snap(mid, (i % 20) - 10, 1000);
        if (i % 3 == 0) {
            snap.trades.push_back(make_trade(Side::BUY, mid, 10));
        } else if (i % 3 == 1) {
            snap.trades.push_back(make_trade(Side::SELL, mid, 10));
        }
        last = strat.compute_quotes(snap.view());
    }

    EXPECT_TRUE(last.should_quote);
    EXPECT_GT(last.bid_price, Ticks{0});
    EXPECT_GT(last.ask_price, last.bid_price);
    EXPECT_GE(last.bid_size, 1);
    EXPECT_GE(last.ask_size, 1);

    AvellanedaStoikovStrategy strat2(cfg);
    QuoteDecision last2;
    for (int i = 0; i < 200; ++i) {
        double mid = 100.0 + 0.5 * std::sin(i * 0.1);
        auto snap = make_snap(mid, (i % 20) - 10, 1000);
        if (i % 3 == 0) {
            snap.trades.push_back(make_trade(Side::BUY, mid, 10));
        } else if (i % 3 == 1) {
            snap.trades.push_back(make_trade(Side::SELL, mid, 10));
        }
        last2 = strat2.compute_quotes(snap.view());
    }

    EXPECT_EQ(last.bid_price, last2.bid_price);
    EXPECT_EQ(last.ask_price, last2.ask_price);
    EXPECT_EQ(last.bid_size, last2.bid_size);
    EXPECT_EQ(last.ask_size, last2.ask_size);
}

}  // namespace
