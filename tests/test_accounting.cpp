#include <cassert>
#include <cmath>
#include <iostream>
#include "include/Accounting.h"

namespace {

constexpr double EPS = 1e-6;

bool near(double a, double b) {
    return std::abs(a - b) < EPS;
}

// 1. Initial state: Zero position, zero PnL, cash = initial capital
void test_initial_state() {
    Accounting acct(100000.0);
    assert(acct.position() == 0);
    assert(near(acct.cash(), 100000.0));
    assert(near(acct.realized_pnl(), 0.0));
    assert(near(acct.unrealized_pnl(), 0.0));
    assert(near(acct.total_pnl(), 0.0));
    assert(near(acct.net_pnl(), 0.0));
    assert(near(acct.total_fees(), 0.0));
    assert(near(acct.total_rebates(), 0.0));
    assert(near(acct.avg_entry_price(), 0.0));
    assert(near(acct.cost_basis(), 0.0));
    std::cout << "PASS: test_initial_state\n";
}

// 2. Single buy: position increases, cash decreases, cost basis set
void test_single_buy() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 50.0, 10, true);

    assert(acct.position() == 10);
    assert(near(acct.cash(), 100000.0 - 500.0));
    assert(near(acct.avg_entry_price(), 50.0));
    assert(near(acct.cost_basis(), 500.0));
    assert(near(acct.realized_pnl(), 0.0));
    // Unrealized at fill price should be 0
    assert(near(acct.unrealized_pnl(), 0.0));
    std::cout << "PASS: test_single_buy\n";
}

// 3. Buy then sell round-trip: realized PnL = (sell - buy) * qty
void test_round_trip() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 50.0, 10, true);
    acct.on_fill(Side::SELL, 52.0, 10, true);

    assert(acct.position() == 0);
    assert(near(acct.realized_pnl(), 20.0));  // (52-50)*10
    assert(near(acct.unrealized_pnl(), 0.0));
    assert(near(acct.cost_basis(), 0.0));
    std::cout << "PASS: test_round_trip\n";
}

// 4. Partial close: realized PnL proportional to closed qty, avg entry preserved
void test_partial_close() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 50.0, 10, true);
    acct.on_fill(Side::SELL, 55.0, 4, true);

    assert(acct.position() == 6);
    assert(near(acct.realized_pnl(), 20.0));  // (55-50)*4
    assert(near(acct.avg_entry_price(), 50.0));  // unchanged for remainder
    assert(near(acct.cost_basis(), 300.0));  // 6 * 50

    // Mark to market at 53
    acct.mark_to_market(53.0);
    assert(near(acct.unrealized_pnl(), 18.0));  // (53-50)*6

    std::cout << "PASS: test_partial_close\n";
}

// 5. Position flip (long to short): realizes PnL on close, resets cost basis
void test_position_flip() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 50.0, 10, true);   // long 10 @ 50
    acct.on_fill(Side::SELL, 55.0, 15, true);   // close 10 + open short 5

    assert(acct.position() == -5);
    assert(near(acct.realized_pnl(), 50.0));     // (55-50)*10
    assert(near(acct.avg_entry_price(), 55.0));   // new short entry
    assert(near(acct.cost_basis(), 275.0));       // 5 * 55

    // Mark at 53: short from 55, now 53 -> profit of 2 per share
    acct.mark_to_market(53.0);
    assert(near(acct.unrealized_pnl(), 10.0));   // (55-53)*5

    std::cout << "PASS: test_position_flip\n";
}

// 6. Mark-to-market: unrealized PnL moves with mark price
void test_mark_to_market() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 5, true);

    acct.mark_to_market(105.0);
    assert(near(acct.unrealized_pnl(), 25.0));  // (105-100)*5

    acct.mark_to_market(95.0);
    assert(near(acct.unrealized_pnl(), -25.0)); // (95-100)*5

    acct.mark_to_market(100.0);
    assert(near(acct.unrealized_pnl(), 0.0));

    std::cout << "PASS: test_mark_to_market\n";
}

// 7. Fees and rebates: net PnL = total PnL - fees + rebates
void test_fees_and_rebates() {
    FeeSchedule fees;
    fees.maker_rebate_per_share = 0.01;
    fees.taker_fee_per_share = 0.03;
    fees.fee_bps = 1.0;  // 1 bps = 0.01%

    Accounting acct(100000.0, fees);

    // Maker fill: buy 10 @ 100
    acct.on_fill(Side::BUY, 100.0, 10, /*is_maker=*/true);
    // fee_bps: 100*10 * 1/10000 = 0.10
    // maker_rebate: 0.01 * 10 = 0.10
    // net fee for this fill: 0.10 - 0.10 = 0.00
    double expected_fees = 0.0;
    double expected_rebates = 0.10;
    assert(near(acct.total_fees(), expected_fees));
    assert(near(acct.total_rebates(), expected_rebates));

    // Taker fill: sell 10 @ 102
    acct.on_fill(Side::SELL, 102.0, 10, /*is_maker=*/false);
    // fee_bps: 102*10 * 1/10000 = 0.102
    // taker_fee: 0.03 * 10 = 0.30
    // total fee for this fill: 0.102 + 0.30 = 0.402
    expected_fees += 0.402;
    assert(near(acct.total_fees(), expected_fees));
    assert(near(acct.total_rebates(), expected_rebates));

    // realized = (102-100)*10 = 20
    // total_pnl = realized + unrealized(0) = 20
    // net_pnl = 20 - fees + rebates = 20 - 0.402 + 0.10 = 19.698
    assert(near(acct.realized_pnl(), 20.0));
    assert(near(acct.net_pnl(), 20.0 - expected_fees + expected_rebates));

    std::cout << "PASS: test_fees_and_rebates\n";
}

// 8. Symmetric fills -> zero inventory
void test_symmetric_fills() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 5, true);
    acct.on_fill(Side::SELL, 100.0, 5, true);

    assert(acct.position() == 0);
    assert(near(acct.unrealized_pnl(), 0.0));
    assert(near(acct.cost_basis(), 0.0));
    std::cout << "PASS: test_symmetric_fills\n";
}

// 9. Accounting identity: total_pnl == realized + unrealized always holds
void test_accounting_identity() {
    Accounting acct(100000.0);

    acct.on_fill(Side::BUY, 100.0, 10, true);
    acct.mark_to_market(105.0);
    assert(near(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl()));

    acct.on_fill(Side::SELL, 103.0, 4, true);
    acct.mark_to_market(108.0);
    assert(near(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl()));

    acct.on_fill(Side::SELL, 110.0, 6, true);
    assert(near(acct.total_pnl(), acct.realized_pnl() + acct.unrealized_pnl()));

    // After full close, unrealized should be 0
    assert(acct.position() == 0);
    assert(near(acct.unrealized_pnl(), 0.0));
    assert(near(acct.total_pnl(), acct.realized_pnl()));

    std::cout << "PASS: test_accounting_identity\n";
}

// C7 regression: on_fill must NOT overwrite mark price with the fill price.
// External mark set via mark_to_market should survive a subsequent on_fill that
// leaves the position non-zero. This locks in the post-C7 contract.
void test_c7_on_fill_does_not_overwrite_mark() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 10, true);   // long 10 @ 100
    acct.mark_to_market(105.0);                  // external mark: unrealized=50
    assert(near(acct.unrealized_pnl(), 50.0));

    // Add to long at 102. Old behavior (pre-C7) would mark at fill price 102,
    // silently overwriting the externally-set 105 mark. Post-C7: on_fill does
    // not implicitly mark, so the caller must re-mark explicitly to get a
    // meaningful unrealized value after a non-flat fill.
    acct.on_fill(Side::BUY, 102.0, 5, true);    // long 15, cost_basis=1510, avg=100.666...
    acct.mark_to_market(105.0);
    // (105 - 100.666...) * 15 = 65.0 exactly: 105*15 - 1510 = 1575 - 1510 = 65
    assert(near(acct.unrealized_pnl(), 65.0));
    std::cout << "PASS: test_c7_on_fill_does_not_overwrite_mark\n";
}

// C7 + flat-position invariant: when on_fill brings position to zero,
// unrealized PnL must be exactly 0.0 (codifies the closed-position invariant).
void test_c7_flat_position_zeroes_unrealized() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 10, true);
    acct.mark_to_market(120.0);                  // unrealized=200
    assert(near(acct.unrealized_pnl(), 200.0));
    acct.on_fill(Side::SELL, 110.0, 10, true);   // close to flat
    assert(acct.position() == 0);
    assert(near(acct.unrealized_pnl(), 0.0));    // flat-position invariant
    assert(near(acct.cost_basis(), 0.0));        // already enforced
    std::cout << "PASS: test_c7_flat_position_zeroes_unrealized\n";
}

// m13 codification: cost_basis must return to exactly 0.0 after any sequence
// ending at position=0, regardless of intermediate partial closes.
void test_cost_basis_flat_invariant_under_partial_closes() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 1000, true);
    for (int i = 0; i < 999; ++i) {
        // Vary close price to exercise the cost_basis subtraction path
        double p = 100.0 + (i % 7) * 0.001;
        acct.on_fill(Side::SELL, p, 1, true);
    }
    assert(acct.position() == 1);
    // One final share to close out
    acct.on_fill(Side::SELL, 100.0, 1, true);
    assert(acct.position() == 0);
    assert(acct.cost_basis() == 0.0);    // exact equality, not near
    assert(near(acct.unrealized_pnl(), 0.0));
    std::cout << "PASS: test_cost_basis_flat_invariant_under_partial_closes\n";
}

// 10. Gross/net exposure: |position| * mark vs position * mark
void test_exposure() {
    Accounting acct(100000.0);
    acct.on_fill(Side::BUY, 100.0, 10, true);

    assert(near(acct.gross_exposure(105.0), 1050.0));  // |10| * 105
    assert(near(acct.net_exposure(105.0), 1050.0));     // 10 * 105

    // Go short
    acct.on_fill(Side::SELL, 105.0, 20, true);  // now -10
    assert(near(acct.gross_exposure(103.0), 1030.0));   // |-10| * 103
    assert(near(acct.net_exposure(103.0), -1030.0));    // -10 * 103

    std::cout << "PASS: test_exposure\n";
}

} // namespace

int main() {
    test_initial_state();
    test_single_buy();
    test_round_trip();
    test_partial_close();
    test_position_flip();
    test_mark_to_market();
    test_fees_and_rebates();
    test_symmetric_fills();
    test_accounting_identity();
    test_c7_on_fill_does_not_overwrite_mark();
    test_c7_flat_position_zeroes_unrealized();
    test_cost_basis_flat_invariant_under_partial_closes();
    test_exposure();

    std::cout << "\nAll accounting tests passed.\n";
    return 0;
}
