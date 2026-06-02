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

TEST(MatchingEngine, test_price_priority) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 5, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, T(101.0), 5, make_ts(2)));
    engine.add_order(Order(3, Side::BUY, T(99.0), 5, make_ts(3)));

    auto fills = engine.match_incoming_order(Side::SELL, T(99.0), 3, 100, make_ts(10));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, 2);
    EXPECT_EQ(fills[0].fill_qty, 3);
    EXPECT_EQ(fills[0].price, T(101.0));
}

TEST(MatchingEngine, test_time_priority) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 5, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, T(100.0), 5, make_ts(2)));

    auto fills = engine.match_incoming_order(Side::SELL, T(100.0), 3, 100, make_ts(10));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, 1);
    EXPECT_EQ(fills[0].fill_qty, 3);
}

TEST(MatchingEngine, test_partial_fill) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 10, make_ts(1)));

    auto fills = engine.match_incoming_order(Side::SELL, T(100.0), 3, 100, make_ts(10));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, 1);
    EXPECT_EQ(fills[0].fill_qty, 3);
    EXPECT_EQ(fills[0].leaves_qty, 7);

    ASSERT_EQ(engine.get_bids().size(), 1u);
    EXPECT_EQ(engine.get_bids()[0].leaves_qty, 7);
    EXPECT_EQ(engine.get_bids()[0].status, OrderStatus::PARTIALLY_FILLED);
}

TEST(MatchingEngine, test_full_fill) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 5, make_ts(1)));

    auto fills = engine.match_incoming_order(Side::SELL, T(100.0), 5, 100, make_ts(10));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, 1);
    EXPECT_EQ(fills[0].fill_qty, 5);
    EXPECT_EQ(fills[0].leaves_qty, 0);

    EXPECT_TRUE(engine.get_bids().empty());
}

TEST(MatchingEngine, test_multi_level_sweep) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(101.0), 3, make_ts(1)));
    engine.add_order(Order(2, Side::BUY, T(100.0), 3, make_ts(2)));
    engine.add_order(Order(3, Side::BUY, T(99.0), 3, make_ts(3)));

    auto fills = engine.match_incoming_order(Side::SELL, T(99.0), 7, 100, make_ts(10));
    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].order_id, 1);
    EXPECT_EQ(fills[0].fill_qty, 3);
    EXPECT_EQ(fills[1].order_id, 2);
    EXPECT_EQ(fills[1].fill_qty, 3);
    EXPECT_EQ(fills[2].order_id, 3);
    EXPECT_EQ(fills[2].fill_qty, 1);
    EXPECT_EQ(fills[2].leaves_qty, 2);

    ASSERT_EQ(engine.get_bids().size(), 1u);
    EXPECT_EQ(engine.get_bids()[0].order_id, 3);
    EXPECT_EQ(engine.get_bids()[0].leaves_qty, 2);
}

TEST(MatchingEngine, test_cancel) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 5, make_ts(1)));
    ASSERT_EQ(engine.get_bids().size(), 1u);

    EXPECT_TRUE(engine.cancel_order(1));
    EXPECT_TRUE(engine.get_bids().empty());

    EXPECT_FALSE(engine.cancel_order(999));
}

TEST(MatchingEngine, test_ask_sorting) {
    MatchingEngine engine;

    engine.add_order(Order(3, Side::SELL, T(103.0), 5, make_ts(3)));
    engine.add_order(Order(1, Side::SELL, T(101.0), 5, make_ts(1)));
    engine.add_order(Order(2, Side::SELL, T(102.0), 5, make_ts(2)));

    const auto& asks = engine.get_asks();
    ASSERT_EQ(asks.size(), 3u);
    EXPECT_EQ(asks[0].price, T(101.0));
    EXPECT_EQ(asks[1].price, T(102.0));
    EXPECT_EQ(asks[2].price, T(103.0));
}

TEST(MatchingEngine, test_bid_sorting) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(99.0), 5, make_ts(1)));
    engine.add_order(Order(3, Side::BUY, T(101.0), 5, make_ts(3)));
    engine.add_order(Order(2, Side::BUY, T(100.0), 5, make_ts(2)));

    const auto& bids = engine.get_bids();
    ASSERT_EQ(bids.size(), 3u);
    EXPECT_EQ(bids[0].price, T(101.0));
    EXPECT_EQ(bids[1].price, T(100.0));
    EXPECT_EQ(bids[2].price, T(99.0));
}

TEST(MatchingEngine, test_no_fill_empty_book) {
    MatchingEngine engine;

    auto fills = engine.match_incoming_order(Side::SELL, T(100.0), 5, 100, make_ts(10));
    EXPECT_TRUE(fills.empty());

    engine.add_order(Order(1, Side::BUY, T(99.0), 5, make_ts(1)));
    fills = engine.match_incoming_order(Side::SELL, T(100.0), 5, 200, make_ts(20));
    EXPECT_TRUE(fills.empty());
}

TEST(MatchingEngine, test_inventory_consistency) {
    MatchingEngine engine;

    engine.add_order(Order(1, Side::BUY, T(100.0), 10, make_ts(1)));
    engine.add_order(Order(2, Side::SELL, T(100.0), 10, make_ts(2)));

    auto buy_fills = engine.match_incoming_order(Side::SELL, T(100.0), 10, 100, make_ts(10));
    auto sell_fills = engine.match_incoming_order(Side::BUY, T(100.0), 10, 200, make_ts(20));

    int net = 0;
    for (const auto& f : buy_fills) {
        net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    }
    for (const auto& f : sell_fills) {
        net += (f.side == Side::BUY) ? f.fill_qty : -f.fill_qty;
    }

    EXPECT_EQ(net, 0);
}

}  // namespace
