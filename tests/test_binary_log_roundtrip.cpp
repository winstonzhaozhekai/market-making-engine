// End-to-end binary log round-trip: deterministic simulator → SpscMdLogger
// file A → MdLogReader → SpscMdLogger file B → cmp -s A B byte-equal.
//
// Also verifies field-equality after reader pass (so even if the writer
// were ever changed in a way that re-serialized identically by accident,
// the field-equality check would still catch semantic drift).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "MarketDataEvent.h"
#include "MarketSimulator.h"
#include "Order.h"
#include "include/MdLogReader.h"
#include "include/SimulationConfig.h"
#include "include/SpscMdLogger.h"

namespace {

std::vector<char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>());
}

int64_t to_ns(std::chrono::system_clock::time_point ts) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        ts.time_since_epoch()).count();
}

void expect_event_equal(const MarketDataEvent& a, const MarketDataEvent& b) {
    EXPECT_EQ(a.instrument, b.instrument);
    EXPECT_EQ(a.sequence_number, b.sequence_number);
    EXPECT_EQ(a.best_bid_price, b.best_bid_price);
    EXPECT_EQ(a.best_ask_price, b.best_ask_price);
    EXPECT_EQ(a.best_bid_size, b.best_bid_size);
    EXPECT_EQ(a.best_ask_size, b.best_ask_size);
    EXPECT_EQ(to_ns(a.timestamp), to_ns(b.timestamp));
    ASSERT_EQ(a.bid_levels.size(), b.bid_levels.size());
    for (std::size_t i = 0; i < a.bid_levels.size(); ++i) {
        EXPECT_EQ(a.bid_levels[i].price, b.bid_levels[i].price);
        EXPECT_EQ(a.bid_levels[i].size, b.bid_levels[i].size);
        EXPECT_EQ(a.bid_levels[i].order_id, b.bid_levels[i].order_id);
        EXPECT_EQ(to_ns(a.bid_levels[i].timestamp), to_ns(b.bid_levels[i].timestamp));
    }
    ASSERT_EQ(a.ask_levels.size(), b.ask_levels.size());
    for (std::size_t i = 0; i < a.ask_levels.size(); ++i) {
        EXPECT_EQ(a.ask_levels[i].price, b.ask_levels[i].price);
        EXPECT_EQ(a.ask_levels[i].size, b.ask_levels[i].size);
    }
    ASSERT_EQ(a.trades.size(), b.trades.size());
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
        EXPECT_EQ(a.trades[i].aggressor_side, b.trades[i].aggressor_side);
        EXPECT_EQ(a.trades[i].price, b.trades[i].price);
        EXPECT_EQ(a.trades[i].size, b.trades[i].size);
        EXPECT_EQ(a.trades[i].trade_id, b.trades[i].trade_id);
    }
    ASSERT_EQ(a.partial_fills.size(), b.partial_fills.size());
    for (std::size_t i = 0; i < a.partial_fills.size(); ++i) {
        EXPECT_EQ(a.partial_fills[i].order_id, b.partial_fills[i].order_id);
        EXPECT_EQ(a.partial_fills[i].price, b.partial_fills[i].price);
    }
    ASSERT_EQ(a.mm_fills.size(), b.mm_fills.size());
}

}  // namespace

TEST(BinaryLogRoundTrip, WriteReadRewriteByteEqual) {
    const auto tmpdir = std::filesystem::temp_directory_path();
    const std::string path_a = (tmpdir / "mme_md_log_a.bin").string();
    const std::string path_b = (tmpdir / "mme_md_log_b.bin").string();
    std::remove(path_a.c_str());
    std::remove(path_b.c_str());

    SimulationConfig cfg;
    cfg.seed = 4242;
    cfg.iterations = 500;
    cfg.quiet = true;

    // Pass 1: simulate + log to A.
    std::vector<MarketDataEvent> events_generated;
    {
        MarketSimulator sim(cfg);
        SpscMdLogger logger(path_a);
        for (int i = 0; i < cfg.iterations; ++i) {
            MarketDataEvent ev = sim.generate_event();
            logger.log_event(ev);
            events_generated.push_back(std::move(ev));
        }
    }

    // Read A back.
    std::vector<MarketDataEvent> events_read;
    {
        MdLogReader reader(path_a);
        while (auto ev = reader.next()) {
            events_read.push_back(std::move(*ev));
        }
    }
    ASSERT_EQ(events_read.size(), events_generated.size());
    for (std::size_t i = 0; i < events_read.size(); ++i) {
        expect_event_equal(events_generated[i], events_read[i]);
    }

    // Pass 2: re-serialize the read-back events to B.
    {
        SpscMdLogger logger(path_b);
        for (const auto& ev : events_read) {
            logger.log_event(ev);
        }
    }

    const auto bytes_a = read_file(path_a);
    const auto bytes_b = read_file(path_b);
    ASSERT_EQ(bytes_a.size(), bytes_b.size())
        << "File A: " << bytes_a.size() << " bytes; File B: " << bytes_b.size();
    EXPECT_EQ(std::memcmp(bytes_a.data(), bytes_b.data(), bytes_a.size()), 0);

    std::remove(path_a.c_str());
    std::remove(path_b.c_str());
}

TEST(BinaryLogRoundTrip, RejectsBadMagic) {
    const auto path = (std::filesystem::temp_directory_path()
                       / "mme_bad_magic.bin").string();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const char garbage[16] = {'N','O','P','E', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        out.write(garbage, sizeof(garbage));
    }
    EXPECT_THROW(MdLogReader r(path), std::runtime_error);
    std::remove(path.c_str());
}
