// M10/2 wiring tests for SimulationMode::ItchReplay.
//
// Each test builds a tiny synthetic ITCH tape in-memory, writes it to a
// temp file, points MarketSimulator at it, drives `generate_event()` and
// asserts that the matching engine reflects the expected book state.

#include "MarketSimulator.h"
#include "include/ItchParser.h"
#include "include/SimulationConfig.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace itch = mme::itch;

namespace {

// Tiny synthetic-tape builder. Methods append length-prefixed messages.
class TapeBuilder {
public:
    void stock_directory(std::uint16_t locate, const char* sym8,
                         std::uint64_t ts_ns) {
        itch::StockDirectory m{};
        m.hdr = { locate, 0, ts_ns };
        std::memcpy(m.stock, sym8, 8);
        write_framed(itch::message_length('R'), [&](std::uint8_t* p) {
            return itch::encode_stock_directory(p, m);
        });
    }
    void add_order(std::uint16_t locate, std::uint64_t ts_ns,
                   std::uint64_t ref, char side, std::uint32_t shares,
                   const char* sym8, std::uint32_t px_x10000) {
        itch::AddOrder m{};
        m.hdr = { locate, 0, ts_ns };
        m.order_ref = ref;
        m.side = side;
        m.shares = shares;
        std::memcpy(m.stock, sym8, 8);
        m.price_x10000 = px_x10000;
        write_framed(itch::message_length('A'), [&](std::uint8_t* p) {
            return itch::encode_add_order(p, m);
        });
    }
    void executed(std::uint16_t locate, std::uint64_t ts_ns,
                  std::uint64_t ref, std::uint32_t shares,
                  std::uint64_t match_no) {
        itch::OrderExecuted m{};
        m.hdr = { locate, 0, ts_ns };
        m.order_ref = ref;
        m.executed_shares = shares;
        m.match_number = match_no;
        write_framed(itch::message_length('E'), [&](std::uint8_t* p) {
            return itch::encode_order_executed(p, m);
        });
    }
    void cancel(std::uint16_t locate, std::uint64_t ts_ns,
                std::uint64_t ref, std::uint32_t canceled) {
        itch::OrderCancel m{};
        m.hdr = { locate, 0, ts_ns };
        m.order_ref = ref;
        m.canceled_shares = canceled;
        write_framed(itch::message_length('X'), [&](std::uint8_t* p) {
            return itch::encode_order_cancel(p, m);
        });
    }
    void del(std::uint16_t locate, std::uint64_t ts_ns,
             std::uint64_t ref) {
        itch::OrderDelete m{};
        m.hdr = { locate, 0, ts_ns };
        m.order_ref = ref;
        write_framed(itch::message_length('D'), [&](std::uint8_t* p) {
            return itch::encode_order_delete(p, m);
        });
    }
    void replace(std::uint16_t locate, std::uint64_t ts_ns,
                 std::uint64_t orig_ref, std::uint64_t new_ref,
                 std::uint32_t new_shares, std::uint32_t new_px_x10000) {
        itch::OrderReplace m{};
        m.hdr = { locate, 0, ts_ns };
        m.orig_order_ref = orig_ref;
        m.new_order_ref  = new_ref;
        m.new_shares     = new_shares;
        m.new_price_x10000 = new_px_x10000;
        write_framed(itch::message_length('U'), [&](std::uint8_t* p) {
            return itch::encode_order_replace(p, m);
        });
    }

    std::string write_to_tempfile(const char* suffix) const {
        const auto dir = std::filesystem::temp_directory_path();
        const std::string path =
            (dir / (std::string("mme_itch_") + suffix + ".bin")).string();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(tape_.data()),
                  static_cast<std::streamsize>(tape_.size()));
        out.close();
        return path;
    }

    const std::vector<std::uint8_t>& bytes() const { return tape_; }

private:
    template <typename EncodeFn>
    void write_framed(std::size_t body_len, EncodeFn encode) {
        const std::size_t off = tape_.size();
        tape_.resize(off + 2 + body_len);
        itch::write_be16(tape_.data() + off,
                         static_cast<std::uint16_t>(body_len));
        encode(tape_.data() + off + 2);
    }

    std::vector<std::uint8_t> tape_;
};

constexpr const char* kSym = "TEST    ";
constexpr std::uint16_t kLocate = 42;

SimulationConfig make_cfg(const std::string& tape_path) {
    SimulationConfig cfg{};
    cfg.instrument     = "TEST";
    cfg.tick_size      = 0.01;
    cfg.initial_price  = 100.0;
    cfg.mode           = SimulationMode::ItchReplay;
    cfg.itch_log_path  = tape_path;
    cfg.itch_symbol    = "TEST";
    cfg.quiet          = true;
    return cfg;
}

}  // namespace

// ---------------------------------------------------------------------

TEST(ItchReplay, AddRestsOrderInEngine) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    // 100 shares BID at $99.99 = 99.9900 = 999900 x10000.
    t.add_order(kLocate, 200ULL, /*ref=*/1, 'B', 100, kSym, 999900);
    const auto path = t.write_to_tempfile("AddRests");

    MarketSimulator sim(make_cfg(path));
    auto ev = sim.generate_event();

    const auto& eng = sim.get_matching_engine();
    ASSERT_FALSE(eng.empty(Side::BUY));
    EXPECT_EQ(eng.best_price(Side::BUY), 9999);  // ticks at $99.99 / 0.01
    EXPECT_EQ(eng.qty_at_depth(Side::BUY, 0), 100);
    EXPECT_EQ(ev.bid_levels.size(), 1u);
    EXPECT_EQ(ev.bid_levels[0].price, 9999);
    EXPECT_EQ(ev.bid_levels[0].size, 100);
}

TEST(ItchReplay, ExecuteAmendsDownAndRemovesAtZero) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.add_order(kLocate, 200ULL, 1, 'B', 100, kSym, 999900);
    t.executed(kLocate, 300ULL, 1, 30, /*match=*/1);
    t.executed(kLocate, 400ULL, 1, 70, /*match=*/2);
    const auto path = t.write_to_tempfile("ExecAmendsZero");

    MarketSimulator sim(make_cfg(path));
    auto ev_add = sim.generate_event();   // A
    auto ev_e1  = sim.generate_event();   // E 30 → 70 left
    const auto& eng = sim.get_matching_engine();
    EXPECT_EQ(eng.qty_at_depth(Side::BUY, 0), 70);
    EXPECT_EQ(ev_e1.trades.size(), 1u);
    EXPECT_EQ(ev_e1.trades[0].size, 30);

    auto ev_e2  = sim.generate_event();   // E 70 → empty
    EXPECT_TRUE(eng.empty(Side::BUY));
    EXPECT_EQ(ev_e2.trades.size(), 1u);
    EXPECT_EQ(ev_e2.trades[0].size, 70);
}

TEST(ItchReplay, CancelXAmendsDownAndPreservesQueuePosition) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.add_order(kLocate, 200ULL, 1, 'B', 200, kSym, 999900);
    t.cancel(kLocate, 300ULL, 1, 50);
    const auto path = t.write_to_tempfile("CancelX");

    MarketSimulator sim(make_cfg(path));
    sim.generate_event();  // A
    sim.generate_event();  // X -50

    const auto& eng = sim.get_matching_engine();
    EXPECT_EQ(eng.qty_at_depth(Side::BUY, 0), 150);
    // Same price-level, single order — order_id at queue pos 0 should
    // still be the tagged ITCH ref 1.
    constexpr std::uint64_t kItchOrderTag = 4ULL << 48;
    EXPECT_EQ(eng.order_id_at_queue_pos(Side::BUY, 0, 0),
              kItchOrderTag | 1ULL);
}

TEST(ItchReplay, DeleteRemovesOrder) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.add_order(kLocate, 200ULL, 1, 'S', 75, kSym, 1000100);  // $100.01
    t.del(kLocate, 300ULL, 1);
    const auto path = t.write_to_tempfile("Delete");

    MarketSimulator sim(make_cfg(path));
    sim.generate_event();
    sim.generate_event();
    EXPECT_TRUE(sim.get_matching_engine().empty(Side::SELL));
}

TEST(ItchReplay, ReplaceCancelsOrigAddsNewAtNewPriceAndSize) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.add_order(kLocate, 200ULL, 1, 'B', 100, kSym, 999900);
    // Replace ref 1 → ref 2 @ $99.98 with 50 shares.
    t.replace(kLocate, 300ULL, 1, 2, 50, 999800);
    const auto path = t.write_to_tempfile("Replace");

    MarketSimulator sim(make_cfg(path));
    sim.generate_event();   // A
    sim.generate_event();   // U
    const auto& eng = sim.get_matching_engine();
    ASSERT_FALSE(eng.empty(Side::BUY));
    EXPECT_EQ(eng.best_price(Side::BUY), 9998);
    EXPECT_EQ(eng.qty_at_depth(Side::BUY, 0), 50);

    // Old ref must be gone.
    constexpr std::uint64_t kItchOrderTag = 4ULL << 48;
    EXPECT_FALSE(eng.contains(kItchOrderTag | 1ULL));
    EXPECT_TRUE (eng.contains(kItchOrderTag | 2ULL));
}

TEST(ItchReplay, IgnoresMessagesForOtherStockLocate) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.stock_directory(99, "OTHER   ", 110ULL);
    // Message for OTHER (locate=99) — must be skipped.
    t.add_order(99, 200ULL, 7, 'B', 999, "OTHER   ", 100000);
    // Then a message for our symbol — must be applied.
    t.add_order(kLocate, 300ULL, 1, 'B', 25, kSym, 999900);
    const auto path = t.write_to_tempfile("OtherLocate");

    MarketSimulator sim(make_cfg(path));
    sim.generate_event();
    const auto& eng = sim.get_matching_engine();
    EXPECT_EQ(eng.num_levels(Side::BUY), 1u);
    EXPECT_EQ(eng.qty_at_depth(Side::BUY, 0), 25);
}

TEST(ItchReplay, ThrowsOnTapeExhausted) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    t.add_order(kLocate, 200ULL, 1, 'B', 10, kSym, 999900);
    const auto path = t.write_to_tempfile("Exhausted");

    MarketSimulator sim(make_cfg(path));
    sim.generate_event();                             // OK
    EXPECT_THROW(sim.generate_event(), std::out_of_range);
}

TEST(ItchReplay, ThrowsOnUnknownSymbol) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    const auto path = t.write_to_tempfile("UnknownSym");

    SimulationConfig cfg = make_cfg(path);
    cfg.itch_symbol = "ZZZZ";  // not present on tape
    EXPECT_THROW({ MarketSimulator s(cfg); }, std::runtime_error);
}

TEST(ItchReplay, ThrowsOnNonZeroLatency) {
    TapeBuilder t;
    t.stock_directory(kLocate, kSym, 100ULL);
    const auto path = t.write_to_tempfile("LatencyForbidden");

    SimulationConfig cfg = make_cfg(path);
    cfg.feed_latency.kind    = mme::LatencyDistribution::Constant;
    cfg.feed_latency.mean_ns = 1000;
    EXPECT_THROW({ MarketSimulator s(cfg); }, std::runtime_error);
}
