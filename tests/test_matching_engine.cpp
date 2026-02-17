#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
#include "MatchingEngine.h"
#include "Order.h"

namespace {

auto make_ts(int ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// 1. Price-time priority: highest bid fills first
void test_price_priority() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 5, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, 101.0, 5, make_ts(2)));
    engine.add_order(Order(3, Side::BUY, 99.0, 5, make_ts(3)));

    auto fills = engine.match_incoming_order(Side::SELL, 99.0, 3, 100, make_ts(10));
    assert(fills.size() == 1);
    assert(fills[0].order_id == 2); // highest bid at 101
    assert(fills[0].fill_qty == 3);
    assert(fills[0].price == 101.0);

    std::cout << "PASS: test_price_priority\n";
}

// 2. Time priority at same price: earlier order fills first
void test_time_priority() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 5, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, 100.0, 5, make_ts(2)));

    auto fills = engine.match_incoming_order(Side::SELL, 100.0, 3, 100, make_ts(10));
    assert(fills.size() == 1);
    assert(fills[0].order_id == 1); // earlier order
    assert(fills[0].fill_qty == 3);

    std::cout << "PASS: test_time_priority\n";
}

// 3. Partial fill: bid for 10, sell for 3 -> leaves_qty=7
void test_partial_fill() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 10, make_ts(1)));

    auto fills = engine.match_incoming_order(Side::SELL, 100.0, 3, 100, make_ts(10));
    assert(fills.size() == 1);
    assert(fills[0].order_id == 1);
    assert(fills[0].fill_qty == 3);
    assert(fills[0].leaves_qty == 7);

    // Order should still be in book
    assert(engine.get_bids().size() == 1);
    assert(engine.get_bids()[0].leaves_qty == 7);
    assert(engine.get_bids()[0].status == OrderStatus::PARTIALLY_FILLED);

    std::cout << "PASS: test_partial_fill\n";
}

// 4. Full fill: bid for 5, sell for 5 -> removed from book
void test_full_fill() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 5, make_ts(1)));

    auto fills = engine.match_incoming_order(Side::SELL, 100.0, 5, 100, make_ts(10));
    assert(fills.size() == 1);
    assert(fills[0].order_id == 1);
    assert(fills[0].fill_qty == 5);
    assert(fills[0].leaves_qty == 0);

    // Order should be removed from book
    assert(engine.get_bids().empty());

    std::cout << "PASS: test_full_fill\n";
}

// 5. Multi-level sweep: aggressive order sweeps across multiple price levels
void test_multi_level_sweep() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 101.0, 3, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, 100.0, 3, make_ts(2)));
    engine.add_order(Order(3, Side::BUY, 99.0, 3, make_ts(3)));

    // Sell 7 @ 99 -> fills B1(3) + B2(3) + B3(1)
    auto fills = engine.match_incoming_order(Side::SELL, 99.0, 7, 100, make_ts(10));
    assert(fills.size() == 3);
    assert(fills[0].order_id == 1);
    assert(fills[0].fill_qty == 3);
    assert(fills[1].order_id == 2);
    assert(fills[1].fill_qty == 3);
    assert(fills[2].order_id == 3);
    assert(fills[2].fill_qty == 1);
    assert(fills[2].leaves_qty == 2);

    // B1 and B2 fully filled and removed, B3 partial
    assert(engine.get_bids().size() == 1);
    assert(engine.get_bids()[0].order_id == 3);
    assert(engine.get_bids()[0].leaves_qty == 2);

    std::cout << "PASS: test_multi_level_sweep\n";
}

// 6. Cancel: add order, cancel it, verify removed
void test_cancel() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 5, make_ts(1)));
    assert(engine.get_bids().size() == 1);

    bool cancelled = engine.cancel_order(1);
    assert(cancelled);
    assert(engine.get_bids().empty());

    // Cancel non-existent order
    bool not_found = engine.cancel_order(999);
    assert(!not_found);

    std::cout << "PASS: test_cancel\n";
}

// 7. Ask sorting: ascending by price
void test_ask_sorting() {
    MatchingEngine engine;

    engine.add_order(Order(3, Side::SELL, 103.0, 5, make_ts(3)));
    engine.add_order(Order(1, Side::SELL, 101.0, 5, make_ts(1)));
    engine.add_order(Order(2, Side::SELL, 102.0, 5, make_ts(2)));

    const auto& asks = engine.get_asks();
    assert(asks.size() == 3);
    assert(asks[0].price == 101.0);
    assert(asks[1].price == 102.0);
    assert(asks[2].price == 103.0);

    std::cout << "PASS: test_ask_sorting\n";
}

// 8. Bid sorting: descending by price
void test_bid_sorting() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 99.0, 5, make_ts(1)));
    engine.add_order(Order(3, Side::BUY, 101.0, 5, make_ts(3)));
    engine.add_order(Order(2, Side::BUY, 100.0, 5, make_ts(2)));

    const auto& bids = engine.get_bids();
    assert(bids.size() == 3);
    assert(bids[0].price == 101.0);
    assert(bids[1].price == 100.0);
    assert(bids[2].price == 99.0);

    std::cout << "PASS: test_bid_sorting\n";
}

// 9. No fill when no match: submit to empty book
void test_no_fill_empty_book() {
    MatchingEngine engine;

    auto fills = engine.match_incoming_order(Side::SELL, 100.0, 5, 100, make_ts(10));
    assert(fills.empty());

    // Also test price mismatch
    engine.add_order(Order(1, Side::BUY, 99.0, 5, make_ts(1)));
    fills = engine.match_incoming_order(Side::SELL, 100.0, 5, 200, make_ts(20));
    assert(fills.empty()); // sell at 100 doesn't match bid at 99

    std::cout << "PASS: test_no_fill_empty_book\n";
}

// 10. Inventory consistency: symmetric buy/sell fills -> net position zero
void test_inventory_consistency() {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, 100.0, 10, make_ts(1)));
    engine.add_order(Order(2, Side::SELL, 100.0, 10, make_ts(2)));

    auto buy_fills = engine.match_incoming_order(Side::SELL, 100.0, 10, 100, make_ts(10));
    auto sell_fills = engine.match_incoming_order(Side::BUY, 100.0, 10, 200, make_ts(20));

    int net = 0;
    for (const auto& f : buy_fills) {
        net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    }
    for (const auto& f : sell_fills) {
        net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    }

    // Buy fills hit our BUY order (+10), sell fills hit our SELL order (-10) = 0
    assert(net == 0);

    std::cout << "PASS: test_inventory_consistency\n";
}

} // namespace

int main() {
    test_price_priority();
    test_time_priority();
    test_partial_fill();
    test_full_fill();
    test_multi_level_sweep();
    test_cancel();
    test_ask_sorting();
    test_bid_sorting();
    test_no_fill_empty_book();
    test_inventory_consistency();

    std::cout << "\nAll matching engine tests passed.\n";
    return 0;
}
