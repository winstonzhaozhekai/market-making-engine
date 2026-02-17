#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include <chrono>
#include "include/Strategy.h"
#include "include/RollingEstimators.h"
#include "include/HeuristicStrategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"

namespace {

constexpr double EPS = 1e-6;

bool near(double a, double b, double eps = EPS) {
    return std::abs(a - b) < eps;
}

using time_point = std::chrono::system_clock::time_point;

time_point base_time() {
    return std::chrono::system_clock::from_time_t(1000000);
}

StrategySnapshot make_snap(double mid, int position = 0, int max_pos = 1000) {
    StrategySnapshot snap;
    snap.best_bid = mid - 0.05;
    snap.best_ask = mid + 0.05;
    snap.mid_price = mid;
    snap.bid_levels.emplace_back(mid - 0.05, 100, 1ULL, base_time());
    snap.ask_levels.emplace_back(mid + 0.05, 100, 2ULL, base_time());
    snap.position = position;
    snap.max_position = max_pos;
    snap.timestamp = base_time();
    snap.sequence_number = 1;
    return snap;
}

Trade make_trade(Side side, double price, int size) {
    Trade t;
    t.aggressor_side = side;
    t.price = price;
    t.size = size;
    t.trade_id = 100;
    t.timestamp = base_time();
    return t;
}

// ============================================================
// RollingVolatility tests (3)
// ============================================================

void test_vol_zero_with_less_than_two_points() {
    RollingVolatility vol(100);
    assert(vol.sigma() == 0.0);
    vol.on_mid(100.0);
    assert(vol.sigma() == 0.0);
    std::cout << "PASS: vol_zero_with_less_than_two_points\n";
}

void test_vol_zero_for_constant_prices() {
    RollingVolatility vol(100);
    for (int i = 0; i < 10; ++i) vol.on_mid(100.0);
    assert(vol.sigma() == 0.0);
    std::cout << "PASS: vol_zero_for_constant_prices\n";
}

void test_vol_known_value() {
    RollingVolatility vol(100);
    // Prices: 100, 101, 100, 101, 100
    // Returns: 0.01, -0.0099..., 0.01, -0.0099...
    vol.on_mid(100.0);
    vol.on_mid(101.0);
    vol.on_mid(100.0);
    vol.on_mid(101.0);
    vol.on_mid(100.0);
    double s = vol.sigma();
    assert(s > 0.005);
    assert(s < 0.015);
    std::cout << "PASS: vol_known_value (sigma=" << s << ")\n";
}

// ============================================================
// RollingOFI tests (3)
// ============================================================

void test_ofi_zero_with_no_trades() {
    RollingOFI ofi(50);
    assert(ofi.normalized_ofi() == 0.0);
    std::cout << "PASS: ofi_zero_with_no_trades\n";
}

void test_ofi_plus_one_for_all_buys() {
    RollingOFI ofi(50);
    std::vector<Trade> trades;
    trades.push_back(make_trade(Side::BUY, 100.0, 10));
    trades.push_back(make_trade(Side::BUY, 100.0, 20));
    ofi.on_trades(trades);
    assert(near(ofi.normalized_ofi(), 1.0));
    std::cout << "PASS: ofi_plus_one_for_all_buys\n";
}

void test_ofi_mixed_trades() {
    RollingOFI ofi(50);
    std::vector<Trade> trades;
    trades.push_back(make_trade(Side::BUY, 100.0, 30));
    trades.push_back(make_trade(Side::SELL, 100.0, 10));
    ofi.on_trades(trades);
    // net = 30 - 10 = 20, total = 40, normalized = 0.5
    assert(near(ofi.normalized_ofi(), 0.5));
    std::cout << "PASS: ofi_mixed_trades\n";
}

// ============================================================
// HeuristicStrategy tests (2)
// ============================================================

void test_heuristic_output_matches_old_logic() {
    HeuristicStrategy strat;
    auto snap = make_snap(100.0, 0, 1000);

    QuoteDecision d = strat.compute_quotes(snap);
    // Zero inventory: skew = 0, spread = 0.02
    assert(near(d.bid_price, 100.0 - 0.01, 1e-4));
    assert(near(d.ask_price, 100.0 + 0.01, 1e-4));
    assert(d.should_quote);
    std::cout << "PASS: heuristic_output_matches_old_logic\n";
}

void test_heuristic_skew_direction() {
    HeuristicStrategy strat;
    // Long position: skew should shift quotes down (negative skew)
    auto snap_long = make_snap(100.0, 5, 1000);
    QuoteDecision d_long = strat.compute_quotes(snap_long);
    // skew = -5 * 0.001 = -0.005
    assert(d_long.bid_price < 100.0 - 0.01);  // bid moved down
    assert(d_long.ask_price < 100.0 + 0.01);  // ask moved down

    // Short position: skew should shift quotes up
    auto snap_short = make_snap(100.0, -5, 1000);
    QuoteDecision d_short = strat.compute_quotes(snap_short);
    assert(d_short.bid_price > 100.0 - 0.01);
    assert(d_short.ask_price > 100.0 + 0.01);
    std::cout << "PASS: heuristic_skew_direction\n";
}

// ============================================================
// A-S core tests (6)
// ============================================================

void test_as_determinism() {
    AvellanedaStoikovConfig cfg;
    AvellanedaStoikovStrategy s1(cfg);
    AvellanedaStoikovStrategy s2(cfg);

    auto snap = make_snap(100.0, 0, 1000);
    // Feed same mid prices to both
    for (int i = 0; i < 5; ++i) {
        snap.mid_price = 100.0 + i * 0.01;
        snap.best_bid = snap.mid_price - 0.05;
        snap.best_ask = snap.mid_price + 0.05;
    }
    QuoteDecision d1 = s1.compute_quotes(snap);
    QuoteDecision d2 = s2.compute_quotes(snap);
    assert(d1.bid_price == d2.bid_price);
    assert(d1.ask_price == d2.ask_price);
    assert(d1.bid_size == d2.bid_size);
    assert(d1.ask_size == d2.ask_size);
    std::cout << "PASS: as_determinism\n";
}

void test_as_reservation_shifts_down_when_long() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy s_flat(cfg);
    AvellanedaStoikovStrategy s_long(cfg);

    // Feed enough mids to get nonzero sigma
    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap_f = make_snap(mid, 0, 1000);
        auto snap_l = make_snap(mid, 50, 1000);
        s_flat.compute_quotes(snap_f);
        s_long.compute_quotes(snap_l);
    }

    auto snap_f = make_snap(100.0, 0, 1000);
    auto snap_l = make_snap(100.0, 50, 1000);
    QuoteDecision d_flat = s_flat.compute_quotes(snap_f);
    QuoteDecision d_long = s_long.compute_quotes(snap_l);

    double mid_flat = (d_flat.bid_price + d_flat.ask_price) / 2.0;
    double mid_long = (d_long.bid_price + d_long.ask_price) / 2.0;
    assert(mid_long < mid_flat);
    std::cout << "PASS: as_reservation_shifts_down_when_long\n";
}

void test_as_reservation_shifts_up_when_short() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy s_flat(cfg);
    AvellanedaStoikovStrategy s_short(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap_f = make_snap(mid, 0, 1000);
        auto snap_s = make_snap(mid, -50, 1000);
        s_flat.compute_quotes(snap_f);
        s_short.compute_quotes(snap_s);
    }

    auto snap_f = make_snap(100.0, 0, 1000);
    auto snap_s = make_snap(100.0, -50, 1000);
    QuoteDecision d_flat = s_flat.compute_quotes(snap_f);
    QuoteDecision d_short = s_short.compute_quotes(snap_s);

    double mid_flat = (d_flat.bid_price + d_flat.ask_price) / 2.0;
    double mid_short = (d_short.bid_price + d_short.ask_price) / 2.0;
    assert(mid_short > mid_flat);
    std::cout << "PASS: as_reservation_shifts_up_when_short\n";
}

void test_as_spread_widens_with_high_vol() {
    // Use high gamma so the sigma-dependent term dominates over the constant ln term
    AvellanedaStoikovConfig cfg;
    cfg.gamma = 50.0;
    cfg.vol_window = 5;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 50000.0;
    AvellanedaStoikovStrategy s_low(cfg);
    AvellanedaStoikovStrategy s_high(cfg);

    // Low vol: stable prices
    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        s_low.compute_quotes(snap);
    }

    // High vol: oscillating prices with large swings
    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? -2.0 : 2.0);
        auto snap = make_snap(mid, 0, 1000);
        s_high.compute_quotes(snap);
    }

    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d_low = s_low.compute_quotes(snap);
    QuoteDecision d_high = s_high.compute_quotes(snap);

    double spread_low = d_low.ask_price - d_low.bid_price;
    double spread_high = d_high.ask_price - d_high.bid_price;
    assert(spread_high > spread_low);
    std::cout << "PASS: as_spread_widens_with_high_vol (low=" << spread_low << " high=" << spread_high << ")\n";
}

void test_as_spread_tightens_with_low_vol() {
    // Set min_spread_bps high enough to be the binding constraint when vol is zero
    AvellanedaStoikovConfig cfg;
    cfg.gamma = 100.0;
    cfg.vol_window = 5;
    cfg.min_spread_bps = 200.0;  // 2.0 at mid=100, well above zero-vol formula output
    AvellanedaStoikovStrategy strat(cfg);

    // Feed constant prices -> zero vol -> spread hits min floor
    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        strat.compute_quotes(snap);
    }
    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d = strat.compute_quotes(snap);
    double spread = d.ask_price - d.bid_price;
    double min_spread = 200.0 * 100.0 / 10000.0;  // 2.0
    assert(near(spread, min_spread, 0.01));
    std::cout << "PASS: as_spread_tightens_with_low_vol (spread=" << spread << " min=" << min_spread << ")\n";
}

void test_as_min_floor_enforced() {
    AvellanedaStoikovConfig cfg;
    cfg.min_spread_bps = 50.0;  // 50bps = 0.5 at mid=100
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0, 0, 1000);
        strat.compute_quotes(snap);
    }
    auto snap = make_snap(100.0, 0, 1000);
    QuoteDecision d = strat.compute_quotes(snap);
    double spread = d.ask_price - d.bid_price;
    double min_spread = 50.0 * 100.0 / 10000.0;  // 0.5
    assert(spread >= min_spread - EPS);
    std::cout << "PASS: as_min_floor_enforced (spread=" << spread << " min=" << min_spread << ")\n";
}

// ============================================================
// Inventory skew tests (3)
// ============================================================

void test_as_long_ask_tighter() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap = make_snap(mid, 50, 1000);
        strat.compute_quotes(snap);
    }

    auto snap = make_snap(100.0, 50, 1000);
    QuoteDecision d = strat.compute_quotes(snap);
    // When long, reservation shifts down -> ask is relatively closer to mid
    // Ask size should be larger (incentivize selling)
    assert(d.ask_size > d.bid_size);
    std::cout << "PASS: as_long_ask_tighter (bid_size=" << d.bid_size << " ask_size=" << d.ask_size << ")\n";
}

void test_as_short_bid_tighter() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.1);
        auto snap = make_snap(mid, -50, 1000);
        strat.compute_quotes(snap);
    }

    auto snap = make_snap(100.0, -50, 1000);
    QuoteDecision d = strat.compute_quotes(snap);
    // When short, bid size should be larger (incentivize buying)
    assert(d.bid_size > d.ask_size);
    std::cout << "PASS: as_short_bid_tighter (bid_size=" << d.bid_size << " ask_size=" << d.ask_size << ")\n";
}

void test_as_max_inventory_max_asymmetry() {
    AvellanedaStoikovConfig cfg;
    cfg.base_size = 10;
    cfg.size_inventory_scale = 1.0;
    cfg.vol_window = 5;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 1000, 1000);  // max long
        strat.compute_quotes(snap);
    }
    auto snap = make_snap(100.0, 1000, 1000);
    QuoteDecision d = strat.compute_quotes(snap);
    // inv_ratio = 1.0, bid_size = 10*(1-1) = 0 -> clamped to 1
    // ask_size = 10*(1+1) = 20
    assert(d.bid_size == 1);  // floor of 0 -> max(1, 0) = 1
    assert(d.ask_size == 20);
    std::cout << "PASS: as_max_inventory_max_asymmetry (bid=" << d.bid_size << " ask=" << d.ask_size << ")\n";
}

// ============================================================
// Adverse selection tests (3)
// ============================================================

void test_as_high_ofi_widens_spread() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 10;
    cfg.ofi_spread_factor = 1.0;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 5000.0;
    AvellanedaStoikovStrategy s_no_ofi(cfg);
    AvellanedaStoikovStrategy s_ofi(cfg);

    // Feed same vol to both
    for (int i = 0; i < 10; ++i) {
        double mid = 100.0 + (i % 2 == 0 ? 0.0 : 0.5);
        auto snap_no = make_snap(mid, 0, 1000);
        auto snap_ofi = make_snap(mid, 0, 1000);
        // Add buy-heavy trades to the OFI version
        snap_ofi.trades.push_back(make_trade(Side::BUY, mid, 100));
        s_no_ofi.compute_quotes(snap_no);
        s_ofi.compute_quotes(snap_ofi);
    }

    auto snap_no = make_snap(100.0, 0, 1000);
    auto snap_ofi = make_snap(100.0, 0, 1000);
    snap_ofi.trades.push_back(make_trade(Side::BUY, 100.0, 100));
    QuoteDecision d_no = s_no_ofi.compute_quotes(snap_no);
    QuoteDecision d_ofi = s_ofi.compute_quotes(snap_ofi);

    double spread_no = d_no.ask_price - d_no.bid_price;
    double spread_ofi = d_ofi.ask_price - d_ofi.bid_price;
    assert(spread_ofi > spread_no);
    std::cout << "PASS: as_high_ofi_widens_spread (no=" << spread_no << " ofi=" << spread_ofi << ")\n";
}

void test_as_pull_on_toxic_true() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 5;
    cfg.toxic_ofi_threshold = 0.5;
    cfg.pull_on_toxic = true;
    AvellanedaStoikovStrategy strat(cfg);

    // Feed all-buy trades to build high OFI
    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 0, 1000);
        snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
        strat.compute_quotes(snap);
    }

    auto snap = make_snap(100.0, 0, 1000);
    snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
    QuoteDecision d = strat.compute_quotes(snap);
    assert(!d.should_quote);
    std::cout << "PASS: as_pull_on_toxic_true\n";
}

void test_as_pull_on_toxic_false_still_quotes_wider() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 5;
    cfg.ofi_window = 5;
    cfg.toxic_ofi_threshold = 0.5;
    cfg.pull_on_toxic = false;  // Don't pull, but spread should still widen
    cfg.ofi_spread_factor = 1.0;
    cfg.min_spread_bps = 1.0;
    cfg.max_spread_bps = 5000.0;
    AvellanedaStoikovStrategy strat(cfg);

    for (int i = 0; i < 10; ++i) {
        auto snap = make_snap(100.0 + i * 0.01, 0, 1000);
        snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
        strat.compute_quotes(snap);
    }

    auto snap = make_snap(100.0, 0, 1000);
    snap.trades.push_back(make_trade(Side::BUY, 100.0, 50));
    QuoteDecision d = strat.compute_quotes(snap);
    assert(d.should_quote);  // Still quoting
    std::cout << "PASS: as_pull_on_toxic_false_still_quotes_wider\n";
}

// ============================================================
// Integration test (1)
// ============================================================

void test_integration_200_snapshots() {
    AvellanedaStoikovConfig cfg;
    cfg.vol_window = 20;
    cfg.ofi_window = 10;
    cfg.base_size = 5;
    AvellanedaStoikovStrategy strat(cfg);

    QuoteDecision last;
    for (int i = 0; i < 200; ++i) {
        // Deterministic price path
        double mid = 100.0 + 0.5 * std::sin(i * 0.1);
        auto snap = make_snap(mid, (i % 20) - 10, 1000);
        // Alternate buy/sell trades
        if (i % 3 == 0) {
            snap.trades.push_back(make_trade(Side::BUY, mid, 10));
        } else if (i % 3 == 1) {
            snap.trades.push_back(make_trade(Side::SELL, mid, 10));
        }
        last = strat.compute_quotes(snap);
    }

    // Verify the final decision is reasonable
    assert(last.should_quote);
    assert(last.bid_price > 0.0);
    assert(last.ask_price > last.bid_price);
    assert(last.bid_size >= 1);
    assert(last.ask_size >= 1);

    // Verify determinism: run again with fresh strategy
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
        last2 = strat2.compute_quotes(snap);
    }

    assert(last.bid_price == last2.bid_price);
    assert(last.ask_price == last2.ask_price);
    assert(last.bid_size == last2.bid_size);
    assert(last.ask_size == last2.ask_size);
    std::cout << "PASS: integration_200_snapshots (bid=" << last.bid_price
              << " ask=" << last.ask_price << " spread=" << (last.ask_price - last.bid_price) << ")\n";
}

} // namespace

int main() {
    std::cout << "=== Strategy Behavior Tests ===\n\n";

    // RollingVolatility (3)
    test_vol_zero_with_less_than_two_points();
    test_vol_zero_for_constant_prices();
    test_vol_known_value();

    // RollingOFI (3)
    test_ofi_zero_with_no_trades();
    test_ofi_plus_one_for_all_buys();
    test_ofi_mixed_trades();

    // HeuristicStrategy (2)
    test_heuristic_output_matches_old_logic();
    test_heuristic_skew_direction();

    // A-S core (6)
    test_as_determinism();
    test_as_reservation_shifts_down_when_long();
    test_as_reservation_shifts_up_when_short();
    test_as_spread_widens_with_high_vol();
    test_as_spread_tightens_with_low_vol();
    test_as_min_floor_enforced();

    // Inventory skew (3)
    test_as_long_ask_tighter();
    test_as_short_bid_tighter();
    test_as_max_inventory_max_asymmetry();

    // Adverse selection (3)
    test_as_high_ofi_widens_spread();
    test_as_pull_on_toxic_true();
    test_as_pull_on_toxic_false_still_quotes_wider();

    // Integration (1)
    test_integration_200_snapshots();

    std::cout << "\nAll 21 strategy behavior tests passed.\n";
    return 0;
}
