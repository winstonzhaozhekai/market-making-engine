#include <gtest/gtest.h>
#include "include/Accounting.h"
#include "include/Instrument.h"

namespace {

constexpr double EPS = 1e-6;
const Instrument kIns{0.01};
Ticks T(double dollars) { return kIns.to_ticks(dollars); }

TEST(Accounting, test_initial_state) {
    Accounting acct(100000.0);
    EXPECT_EQ(acct.position(), 0);
    EXPECT_NEAR(acct.cash(), 100000.0, EPS);
    EXPECT_NEAR(acct.realized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.total_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.net_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.total_fees(), 0.0, EPS);
    EXPECT_NEAR(acct.total_rebates(), 0.0, EPS);
    EXPECT_NEAR(acct.avg_entry_price(), 0.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 0.0, EPS);
}

TEST(Accounting, test_single_buy) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(50.0), 10, true);

    EXPECT_EQ(acct.position(), 10);
    EXPECT_NEAR(acct.cash(), 100000.0 - 500.0, EPS);
    EXPECT_NEAR(acct.avg_entry_price(), 50.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 500.0, EPS);
    EXPECT_NEAR(acct.realized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
}

TEST(Accounting, test_round_trip) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(50.0), 10, true);
    acct.on_fill(Side::SELL, T(52.0), 10, true);

    EXPECT_EQ(acct.position(), 0);
    EXPECT_NEAR(acct.realized_pnl(), 20.0, EPS);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 0.0, EPS);
}

TEST(Accounting, test_partial_close) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(50.0), 10, true);
    acct.on_fill(Side::SELL, T(55.0), 4, true);

    EXPECT_EQ(acct.position(), 6);
    EXPECT_NEAR(acct.realized_pnl(), 20.0, EPS);
    EXPECT_NEAR(acct.avg_entry_price(), 50.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 300.0, EPS);

    acct.mark_to_market(53.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 18.0, EPS);
}

TEST(Accounting, test_position_flip) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(50.0), 10, true);
    acct.on_fill(Side::SELL, T(55.0), 15, true);

    EXPECT_EQ(acct.position(), -5);
    EXPECT_NEAR(acct.realized_pnl(), 50.0, EPS);
    EXPECT_NEAR(acct.avg_entry_price(), 55.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 275.0, EPS);

    acct.mark_to_market(53.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 10.0, EPS);
}

TEST(Accounting, test_mark_to_market) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 5, true);

    acct.mark_to_market(105.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 25.0, EPS);

    acct.mark_to_market(95.0);
    EXPECT_NEAR(acct.unrealized_pnl(), -25.0, EPS);

    acct.mark_to_market(100.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
}

TEST(Accounting, test_fees_and_rebates) {
    FeeSchedule fees;
    fees.maker_rebate_per_share = 0.01;
    fees.taker_fee_per_share = 0.03;
    fees.fee_bps = 1.0;

    Accounting acct(100000.0, fees);

    acct.on_fill(Side::BUY, T(100.0), 10, /*is_maker=*/true);
    double expected_fees = 0.0;
    double expected_rebates = 0.10;
    EXPECT_NEAR(acct.total_fees(), expected_fees, EPS);
    EXPECT_NEAR(acct.total_rebates(), expected_rebates, EPS);

    acct.on_fill(Side::SELL, T(102.0), 10, /*is_maker=*/false);
    expected_fees += 0.402;
    EXPECT_NEAR(acct.total_fees(), expected_fees, EPS);
    EXPECT_NEAR(acct.total_rebates(), expected_rebates, EPS);

    EXPECT_NEAR(acct.realized_pnl(), 20.0, EPS);
    EXPECT_NEAR(acct.net_pnl(), 20.0 - expected_fees + expected_rebates, EPS);
}

TEST(Accounting, test_symmetric_fills) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 5, true);
    acct.on_fill(Side::SELL, T(100.0), 5, true);

    EXPECT_EQ(acct.position(), 0);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 0.0, EPS);
}

TEST(Accounting, test_accounting_identity) {
    Accounting acct(100000.0);

    acct.on_fill(Side::BUY, T(100.0), 10, true);
    acct.mark_to_market(105.0);
    EXPECT_NEAR(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl(), EPS);

    acct.on_fill(Side::SELL, T(103.0), 4, true);
    acct.mark_to_market(108.0);
    EXPECT_NEAR(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl(), EPS);

    acct.on_fill(Side::SELL, T(110.0), 6, true);
    EXPECT_NEAR(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl(), EPS);

    EXPECT_EQ(acct.position(), 0);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.total_pnl(), acct.realized_pnl(), EPS);
}

// C7 regression: on_fill must NOT overwrite mark price with the fill price.
TEST(Accounting, test_c7_on_fill_does_not_overwrite_mark) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 10, true);
    acct.mark_to_market(105.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 50.0, EPS);

    acct.on_fill(Side::BUY, T(102.0), 5, true);
    acct.mark_to_market(105.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 65.0, EPS);
}

// C7 + flat-position invariant: position->0 implies unrealized = 0 exactly.
TEST(Accounting, test_c7_flat_position_zeroes_unrealized) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 10, true);
    acct.mark_to_market(120.0);
    EXPECT_NEAR(acct.unrealized_pnl(), 200.0, EPS);
    acct.on_fill(Side::SELL, T(110.0), 10, true);
    EXPECT_EQ(acct.position(), 0);
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
    EXPECT_NEAR(acct.cost_basis(), 0.0, EPS);
}

// m13 codification: cost_basis returns to exactly 0.0 after position flat.
TEST(Accounting, test_cost_basis_flat_invariant_under_partial_closes) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 1000, true);
    for (int i = 0; i < 999; ++i) {
        // Prices wander around 100.00 within a few ticks. At tick_size=0.01,
        // values like 100.001 snap to 10000 ticks; values like 100.006 snap to
        // 10001 ticks — still exercises the cost-basis invariant under mixed
        // closing prices, which is what m13 codifies.
        double p = 100.0 + (i % 7) * 0.001;
        acct.on_fill(Side::SELL, T(p), 1, true);
    }
    EXPECT_EQ(acct.position(), 1);
    acct.on_fill(Side::SELL, T(100.0), 1, true);
    EXPECT_EQ(acct.position(), 0);
    EXPECT_DOUBLE_EQ(acct.cost_basis(), 0.0);  // exact equality, not near
    EXPECT_NEAR(acct.unrealized_pnl(), 0.0, EPS);
}

TEST(Accounting, test_exposure) {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, T(100.0), 10, true);

    EXPECT_NEAR(acct.gross_exposure(105.0), 1050.0, EPS);
    EXPECT_NEAR(acct.net_exposure(105.0), 1050.0, EPS);

    acct.on_fill(Side::SELL, T(105.0), 20, true);
    EXPECT_NEAR(acct.gross_exposure(103.0), 1030.0, EPS);
    EXPECT_NEAR(acct.net_exposure(103.0), -1030.0, EPS);
}

}  // namespace
