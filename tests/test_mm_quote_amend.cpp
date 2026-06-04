#include <gtest/gtest.h>

#include <chrono>
#include <memory>

#include "MarketDataEvent.h"
#include "MarketMaker.h"
#include "MarketSimulator.h"
#include "Order.h"
#include "include/Instrument.h"
#include "include/Logger.h"
#include "include/RiskManager.h"
#include "include/SimulationConfig.h"
#include "include/Strategy.h"

namespace {

// Strategy whose decisions are dictated by the test. compute_quotes
// returns whatever the test last set in `next`. No internal state.
class StubStrategy : public Strategy {
public:
    QuoteDecision next{};
    QuoteDecision compute_quotes(const StrategySnapshot&) override { return next; }
    const char* name() const override { return "stub"; }
};

struct Fixture {
    SimulationConfig            cfg;
    std::unique_ptr<MarketSimulator> sim;
    StubStrategy*               stub = nullptr;   // non-owning
    std::unique_ptr<MarketMaker> mm;
    Instrument                  ins;

    Fixture() {
        cfg.seed = 1;
        cfg.iterations = 0;
        cfg.quiet = true;
        // MM quote/amend regression tests assume an empty matching
        // engine at construction. Under QueueReactive default the
        // engine is pre-seeded with HLR initial_queue orders, which
        // changes interaction outcomes. Pin to Legacy.
        cfg.lob_model = LobModel::Legacy;
        sim = std::make_unique<MarketSimulator>(cfg);
        ins = sim->instrument_meta();
        auto owned = std::make_unique<StubStrategy>();
        stub = owned.get();
        mm = std::make_unique<MarketMaker>(ins, RiskConfig{}, std::move(owned),
                                           std::make_unique<NullLogger>());
    }

    const MatchingEngine& engine() const { return sim->get_matching_engine(); }
};

std::chrono::system_clock::time_point at(int64_t ns) {
    using sc = std::chrono::system_clock;
    return sc::time_point{
        std::chrono::duration_cast<sc::duration>(std::chrono::nanoseconds{ns})};
}

MarketDataEvent make_md(Ticks bid_px, Ticks ask_px, int64_t seq, int64_t ts_ns) {
    MarketDataEvent md;
    md.instrument = "TEST";
    md.best_bid_price = bid_px;
    md.best_ask_price = ask_px;
    md.best_bid_size = 100;
    md.best_ask_size = 100;
    md.bid_levels.emplace_back(bid_px, 100, 0ULL, at(ts_ns));
    md.ask_levels.emplace_back(ask_px, 100, 0ULL, at(ts_ns));
    md.timestamp = at(ts_ns);
    md.sequence_number = seq;
    return md;
}

QuoteDecision quote(Ticks bid_px, Ticks ask_px, int bid_sz, int ask_sz) {
    QuoteDecision d;
    d.bid_price = bid_px;
    d.ask_price = ask_px;
    d.bid_size  = bid_sz;
    d.ask_size  = ask_sz;
    d.should_quote = true;
    return d;
}

// ============================================================
// Amend matrix
// ============================================================

TEST(MMQuoteAmend, unchanged_quote_emits_no_engine_message) {
    Fixture f;
    Ticks bp = f.ins.to_ticks(99.95), ap = f.ins.to_ticks(100.05);

    f.stub->next = quote(bp, ap, 5, 5);
    f.mm->on_market_data(make_md(bp, ap, 1, 1'000'000), *f.sim);

    ASSERT_EQ(f.engine().num_orders(Side::BUY),  1u);
    ASSERT_EQ(f.engine().num_orders(Side::SELL), 1u);
    uint64_t bid_id_1 = f.engine().order_id_at_queue_pos(Side::BUY,  0, 0);
    uint64_t ask_id_1 = f.engine().order_id_at_queue_pos(Side::SELL, 0, 0);

    // Tick 2 — same quote. Engine state must be byte-identical.
    f.mm->on_market_data(make_md(bp, ap, 2, 2'000'000), *f.sim);

    EXPECT_EQ(f.engine().num_orders(Side::BUY),  1u);
    EXPECT_EQ(f.engine().num_orders(Side::SELL), 1u);
    EXPECT_EQ(f.engine().order_id_at_queue_pos(Side::BUY,  0, 0), bid_id_1);
    EXPECT_EQ(f.engine().order_id_at_queue_pos(Side::SELL, 0, 0), ask_id_1);
    EXPECT_EQ(f.engine().leaves_qty_of(bid_id_1), 5);
    EXPECT_EQ(f.engine().leaves_qty_of(ask_id_1), 5);
}

TEST(MMQuoteAmend, smaller_size_at_same_price_preserves_queue_position) {
    Fixture f;
    Ticks bp = f.ins.to_ticks(99.95), ap = f.ins.to_ticks(100.05);

    f.stub->next = quote(bp, ap, 10, 10);
    f.mm->on_market_data(make_md(bp, ap, 1, 1'000'000), *f.sim);

    uint64_t mm_bid = f.engine().order_id_at_queue_pos(Side::BUY, 0, 0);

    // Plant a competing external order behind the MM at the same bid price.
    constexpr uint64_t kExtId = 99999ULL;
    Order external(kExtId, Side::BUY, bp, 4, at(1'500'000));
    ASSERT_EQ(f.sim->submit_order(external, OrderType::POST_ONLY),
              OrderStatus::ACKNOWLEDGED);
    ASSERT_EQ(f.engine().order_id_at_queue_pos(Side::BUY, 0, 0), mm_bid);
    ASSERT_EQ(f.engine().order_id_at_queue_pos(Side::BUY, 0, 1), kExtId);

    // Tick 2 — MM downsizes its bid (same price, smaller size).
    f.stub->next = quote(bp, ap, 6, 10);
    f.mm->on_market_data(make_md(bp, ap, 2, 2'000'000), *f.sim);

    // Queue order preserved: MM still ahead, external still behind.
    EXPECT_EQ(f.engine().order_id_at_queue_pos(Side::BUY, 0, 0), mm_bid);
    EXPECT_EQ(f.engine().order_id_at_queue_pos(Side::BUY, 0, 1), kExtId);
    EXPECT_EQ(f.engine().leaves_qty_of(mm_bid), 6);
    EXPECT_EQ(f.engine().leaves_qty_of(kExtId), 4);
    EXPECT_EQ(f.engine().qty_at_depth(Side::BUY, 0), 10);
}

TEST(MMQuoteAmend, price_change_loses_position_at_new_level) {
    Fixture f;
    Ticks bp = f.ins.to_ticks(99.95), ap = f.ins.to_ticks(100.05);

    f.stub->next = quote(bp, ap, 5, 5);
    f.mm->on_market_data(make_md(bp, ap, 1, 1'000'000), *f.sim);
    uint64_t mm_bid = f.engine().order_id_at_queue_pos(Side::BUY, 0, 0);

    // Tick 2 — MM moves bid down one tick.
    Ticks new_bp = bp - 1;
    f.stub->next = quote(new_bp, ap, 5, 5);
    f.mm->on_market_data(make_md(new_bp, ap, 2, 2'000'000), *f.sim);

    // Single price level at the new bid; MM order is the sole occupant
    // (queue was effectively reset by the amend-with-price-change).
    EXPECT_EQ(f.engine().num_levels(Side::BUY), 1u);
    EXPECT_EQ(f.engine().best_price(Side::BUY), new_bp);
    EXPECT_EQ(f.engine().order_id_at_queue_pos(Side::BUY, 0, 0), mm_bid);
    EXPECT_EQ(f.engine().leaves_qty_of(mm_bid), 5);
}

TEST(MMQuoteAmend, should_quote_false_cancels_existing) {
    Fixture f;
    Ticks bp = f.ins.to_ticks(99.95), ap = f.ins.to_ticks(100.05);

    f.stub->next = quote(bp, ap, 5, 5);
    f.mm->on_market_data(make_md(bp, ap, 1, 1'000'000), *f.sim);
    ASSERT_EQ(f.engine().num_orders(Side::BUY),  1u);
    ASSERT_EQ(f.engine().num_orders(Side::SELL), 1u);

    // Tick 2 — strategy pulls (should_quote=false).
    QuoteDecision pull{};
    pull.should_quote = false;
    f.stub->next = pull;
    f.mm->on_market_data(make_md(bp, ap, 2, 2'000'000), *f.sim);

    EXPECT_EQ(f.engine().num_orders(Side::BUY),  0u);
    EXPECT_EQ(f.engine().num_orders(Side::SELL), 0u);
}

} // namespace
