#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include "MatchingEngine.h"
#include "Order.h"
#include "include/Instrument.h"

namespace {

const Instrument kIns{0.01};
Ticks T(double dollars) { return kIns.to_ticks(dollars); }

auto make_ts(int ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

// Ergonomics: maker rests via POST_ONLY (the realistic MM intent),
// aggressor sweeps via IOC. Distinct id spaces to avoid collisions in
// engine.lookup_.
constexpr uint64_t kAggrTag = 1ULL << 60;

SubmitResult post(MatchingEngine& e, uint64_t id, Side s, Ticks px, int qty,
                  std::chrono::system_clock::time_point ts) {
    return e.add_order(Order(id, s, px, qty, ts), OrderType::POST_ONLY);
}

SubmitResult ioc(MatchingEngine& e, uint64_t trade_id, Side s, Ticks px,
                 int qty, std::chrono::system_clock::time_point ts) {
    Order o(kAggrTag | trade_id, s, px, qty, ts);
    return e.add_order(std::move(o), OrderType::IOC, trade_id);
}

SubmitResult limit(MatchingEngine& e, uint64_t id, uint64_t trade_id, Side s,
                   Ticks px, int qty,
                   std::chrono::system_clock::time_point ts) {
    return e.add_order(Order(id, s, px, qty, ts), OrderType::LIMIT, trade_id);
}

// ---- Original test coverage, ported to the new API ---------------------

TEST(MatchingEngine, price_priority) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));
    post(engine, 2, Side::BUY, T(101.0), 5, make_ts(2));
    post(engine, 3, Side::BUY, T( 99.0), 5, make_ts(3));

    auto r = ioc(engine, /*trade=*/100, Side::SELL, T(99.0), 3, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].order_id, 2u);
    EXPECT_EQ(r.fills[0].fill_qty, 3);
    EXPECT_EQ(r.fills[0].price, T(101.0));
}

TEST(MatchingEngine, time_priority) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));
    post(engine, 2, Side::BUY, T(100.0), 5, make_ts(2));

    auto r = ioc(engine, 100, Side::SELL, T(100.0), 3, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].order_id, 1u);
    EXPECT_EQ(r.fills[0].fill_qty, 3);
}

TEST(MatchingEngine, partial_fill) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 10, make_ts(1));

    auto r = ioc(engine, 100, Side::SELL, T(100.0), 3, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].order_id, 1u);
    EXPECT_EQ(r.fills[0].fill_qty, 3);
    EXPECT_EQ(r.fills[0].leaves_qty, 7);

    EXPECT_EQ(engine.num_orders(Side::BUY), 1u);
    EXPECT_EQ(engine.leaves_qty_of(1), 7);
    EXPECT_EQ(engine.status_of(1), OrderStatus::PARTIALLY_FILLED);
}

TEST(MatchingEngine, full_fill) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));

    auto r = ioc(engine, 100, Side::SELL, T(100.0), 5, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].fill_qty, 5);
    EXPECT_EQ(r.fills[0].leaves_qty, 0);

    EXPECT_TRUE(engine.empty(Side::BUY));
    EXPECT_FALSE(engine.contains(1));
}

TEST(MatchingEngine, multi_level_sweep) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(101.0), 3, make_ts(1));
    post(engine, 2, Side::BUY, T(100.0), 3, make_ts(2));
    post(engine, 3, Side::BUY, T( 99.0), 3, make_ts(3));

    auto r = ioc(engine, 100, Side::SELL, T(99.0), 7, make_ts(10));
    ASSERT_EQ(r.fills.size(), 3u);
    EXPECT_EQ(r.fills[0].order_id, 1u); EXPECT_EQ(r.fills[0].fill_qty, 3);
    EXPECT_EQ(r.fills[1].order_id, 2u); EXPECT_EQ(r.fills[1].fill_qty, 3);
    EXPECT_EQ(r.fills[2].order_id, 3u); EXPECT_EQ(r.fills[2].fill_qty, 1);
    EXPECT_EQ(r.fills[2].leaves_qty, 2);

    EXPECT_EQ(engine.num_orders(Side::BUY), 1u);
    EXPECT_EQ(engine.best_price(Side::BUY), T(99.0));
    EXPECT_EQ(engine.leaves_qty_of(3), 2);
}

TEST(MatchingEngine, cancel) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));
    EXPECT_EQ(engine.num_orders(Side::BUY), 1u);

    EXPECT_TRUE(engine.cancel_order(1));
    EXPECT_TRUE(engine.empty(Side::BUY));
    EXPECT_FALSE(engine.contains(1));

    EXPECT_FALSE(engine.cancel_order(999));
}

TEST(MatchingEngine, ask_sorting) {
    MatchingEngine engine;
    post(engine, 3, Side::SELL, T(103.0), 5, make_ts(3));
    post(engine, 1, Side::SELL, T(101.0), 5, make_ts(1));
    post(engine, 2, Side::SELL, T(102.0), 5, make_ts(2));

    ASSERT_EQ(engine.num_levels(Side::SELL), 3u);
    EXPECT_EQ(engine.price_at_depth(Side::SELL, 0), T(101.0));
    EXPECT_EQ(engine.price_at_depth(Side::SELL, 1), T(102.0));
    EXPECT_EQ(engine.price_at_depth(Side::SELL, 2), T(103.0));
}

TEST(MatchingEngine, bid_sorting) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T( 99.0), 5, make_ts(1));
    post(engine, 3, Side::BUY, T(101.0), 5, make_ts(3));
    post(engine, 2, Side::BUY, T(100.0), 5, make_ts(2));

    ASSERT_EQ(engine.num_levels(Side::BUY), 3u);
    EXPECT_EQ(engine.price_at_depth(Side::BUY, 0), T(101.0));
    EXPECT_EQ(engine.price_at_depth(Side::BUY, 1), T(100.0));
    EXPECT_EQ(engine.price_at_depth(Side::BUY, 2), T( 99.0));
}

TEST(MatchingEngine, no_fill_empty_book) {
    MatchingEngine engine;
    auto r = ioc(engine, 100, Side::SELL, T(100.0), 5, make_ts(10));
    EXPECT_TRUE(r.fills.empty());
    EXPECT_EQ(r.status, OrderStatus::CANCELED);

    post(engine, 1, Side::BUY, T(99.0), 5, make_ts(1));
    auto r2 = ioc(engine, 200, Side::SELL, T(100.0), 5, make_ts(20));
    EXPECT_TRUE(r2.fills.empty());
}

TEST(MatchingEngine, inventory_consistency) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY,  T(100.0), 10, make_ts(1));
    post(engine, 2, Side::SELL, T(101.0), 10, make_ts(2));

    auto sell_fills = ioc(engine, 100, Side::SELL, T(100.0), 10, make_ts(10)).fills;
    auto buy_fills  = ioc(engine, 200, Side::BUY,  T(101.0), 10, make_ts(20)).fills;

    int net = 0;
    for (const auto& f : sell_fills) net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    for (const auto& f : buy_fills)  net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    EXPECT_EQ(net, 0);
}

// ---- New coverage: crossed-insert rejection (closes M2) ----------------

TEST(MatchingEngine, post_only_crossed_buy_rejected) {
    MatchingEngine engine;
    post(engine, 1, Side::SELL, T(100.0), 5, make_ts(1));

    auto r = post(engine, 2, Side::BUY, T(100.0), 5, make_ts(2));
    EXPECT_EQ(r.status, OrderStatus::REJECTED);
    EXPECT_TRUE(r.fills.empty());
    EXPECT_FALSE(engine.contains(2));
    EXPECT_TRUE(engine.empty(Side::BUY));
    EXPECT_EQ(engine.leaves_qty_of(1), 5);  // resting ask untouched

    // Non-marketable bid one tick below — accepted.
    auto r2 = post(engine, 3, Side::BUY, T(99.99), 5, make_ts(3));
    EXPECT_EQ(r2.status, OrderStatus::ACKNOWLEDGED);
    EXPECT_TRUE(engine.contains(3));
}

TEST(MatchingEngine, post_only_crossed_sell_rejected) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));

    auto r = post(engine, 2, Side::SELL, T(100.0), 5, make_ts(2));
    EXPECT_EQ(r.status, OrderStatus::REJECTED);
    EXPECT_FALSE(engine.contains(2));
}

// ---- New coverage: LIMIT match-then-rest --------------------------------

TEST(MatchingEngine, limit_aggresses_and_rests_residual) {
    MatchingEngine engine;
    post(engine, 1, Side::SELL, T(100.0), 5, make_ts(1));

    // Buy 10 @ 101 — sweeps the 5 @ 100, residual 5 rests at 101.
    auto r = limit(engine, /*id=*/2, /*trade=*/500, Side::BUY,
                   T(101.0), 10, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].order_id, 1u);     // maker
    EXPECT_EQ(r.fills[0].fill_qty, 5);
    EXPECT_EQ(r.status, OrderStatus::PARTIALLY_FILLED);

    EXPECT_TRUE(engine.empty(Side::SELL));
    EXPECT_TRUE(engine.contains(2));
    EXPECT_EQ(engine.best_price(Side::BUY), T(101.0));
    EXPECT_EQ(engine.leaves_qty_of(2), 5);
}

TEST(MatchingEngine, ioc_aggresses_and_drops_residual) {
    MatchingEngine engine;
    post(engine, 1, Side::SELL, T(100.0), 5, make_ts(1));

    auto r = ioc(engine, /*trade=*/500, Side::BUY, T(101.0), 10, make_ts(10));
    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].fill_qty, 5);
    EXPECT_EQ(r.status, OrderStatus::PARTIALLY_FILLED);

    EXPECT_TRUE(engine.empty(Side::BUY));   // IOC residual NOT rested
    EXPECT_TRUE(engine.empty(Side::SELL));
}

// ---- New coverage: amend semantics --------------------------------------

TEST(MatchingEngine, amend_same_price_smaller_qty_preserves_position) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 10, make_ts(1));
    post(engine, 2, Side::BUY, T(100.0), 10, make_ts(2));

    ASSERT_EQ(engine.order_id_at_queue_pos(Side::BUY, 0, 0), 1u);

    EXPECT_TRUE(engine.amend_order(1, T(100.0), 4, make_ts(5)));
    EXPECT_EQ(engine.leaves_qty_of(1), 4);
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 0, 0), 1u);  // still head
    EXPECT_EQ(engine.qty_at_depth(Side::BUY, 0), 14);              // 4 + 10

    // Sweep proves position preserved: id=1 fills first.
    auto r = ioc(engine, 100, Side::SELL, T(100.0), 5, make_ts(10));
    ASSERT_EQ(r.fills.size(), 2u);
    EXPECT_EQ(r.fills[0].order_id, 1u);
    EXPECT_EQ(r.fills[0].fill_qty, 4);
    EXPECT_EQ(r.fills[1].order_id, 2u);
    EXPECT_EQ(r.fills[1].fill_qty, 1);
}

TEST(MatchingEngine, amend_same_price_larger_qty_loses_position) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 10, make_ts(1));
    post(engine, 2, Side::BUY, T(100.0), 10, make_ts(2));

    EXPECT_TRUE(engine.amend_order(1, T(100.0), 15, make_ts(5)));
    EXPECT_EQ(engine.leaves_qty_of(1), 15);
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 0, 0), 2u);  // id=2 now head
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 0, 1), 1u);
}

TEST(MatchingEngine, amend_price_change_loses_position) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));
    post(engine, 2, Side::BUY, T(100.0), 5, make_ts(2));
    post(engine, 3, Side::BUY, T( 99.0), 5, make_ts(3));

    EXPECT_TRUE(engine.amend_order(1, T(99.0), 5, make_ts(5)));

    EXPECT_EQ(engine.orders_at_depth(Side::BUY, 0), 1u);
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 0, 0), 2u);

    EXPECT_EQ(engine.orders_at_depth(Side::BUY, 1), 2u);
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 1, 0), 3u);  // first there
    EXPECT_EQ(engine.order_id_at_queue_pos(Side::BUY, 1, 1), 1u);  // appended
}

TEST(MatchingEngine, amend_to_crossing_price_rejected) {
    MatchingEngine engine;
    post(engine, 1, Side::BUY,  T(100.0), 5, make_ts(1));
    post(engine, 2, Side::SELL, T(102.0), 5, make_ts(2));

    // Amend bid up across opposite best — rejected.
    EXPECT_FALSE(engine.amend_order(1, T(102.0), 5, make_ts(5)));
    EXPECT_EQ(engine.leaves_qty_of(1), 5);
    EXPECT_EQ(engine.best_price(Side::BUY), T(100.0));
    EXPECT_EQ(engine.best_price(Side::SELL), T(102.0));
}

TEST(MatchingEngine, amend_nonexistent_returns_false) {
    MatchingEngine engine;
    EXPECT_FALSE(engine.amend_order(42, T(100.0), 5, make_ts(1)));

    post(engine, 1, Side::BUY, T(100.0), 5, make_ts(1));
    EXPECT_FALSE(engine.amend_order(1, T(100.0),  0, make_ts(2)));  // bad qty
    EXPECT_FALSE(engine.amend_order(1, T(100.0), -3, make_ts(2)));
    EXPECT_FALSE(engine.amend_order(1, Ticks{0}, 5, make_ts(2)));   // bad price
}

}  // namespace
