#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "MarketSimulator.h"
#include "include/SimulationConfig.h"

namespace {

struct RunDigest {
    int processed = 0;
    uint64_t checksum = 1469598103934665603ULL;
    double avg_bid = 0.0;
    double avg_ask = 0.0;
};

struct RunCapture {
    RunDigest digest;
    std::vector<MarketDataEvent> events;
};

uint64_t update_fnv1a(uint64_t hash, const std::string& data) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (unsigned char ch : data) {
        hash ^= ch;
        hash *= kPrime;
    }
    return hash;
}

std::string event_fingerprint(const MarketDataEvent& md) {
    std::ostringstream fp;
    fp << md.sequence_number << "|"
       << std::fixed << std::setprecision(6)
       << md.best_bid_price << "|"
       << md.best_ask_price << "|"
       << md.best_bid_size << "|"
       << md.best_ask_size;
    for (const auto& trade : md.trades) {
        fp << "|T:" << (trade.aggressor_side == Side::BUY ? "BUY" : "SELL") << ":"
           << std::fixed << std::setprecision(6) << trade.price << ":" << trade.size;
    }
    for (const auto& fill : md.partial_fills) {
        fp << "|F:" << fill.order_id << ":"
           << std::fixed << std::setprecision(6) << fill.price << ":"
           << fill.filled_size << ":" << fill.remaining_size;
    }
    return fp.str();
}

int64_t to_millis(const std::chrono::system_clock::time_point& ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch()).count();
}

bool nearly_equal(double a, double b, double epsilon = 1e-12) {
    return std::abs(a - b) <= epsilon;
}

void expect_order_level_equal(const OrderLevel& lhs, const OrderLevel& rhs) {
    EXPECT_TRUE(nearly_equal(lhs.price, rhs.price));
    EXPECT_EQ(lhs.size, rhs.size);
    EXPECT_EQ(lhs.order_id, rhs.order_id);
    EXPECT_EQ(to_millis(lhs.timestamp), to_millis(rhs.timestamp));
}

void expect_trade_equal(const Trade& lhs, const Trade& rhs) {
    EXPECT_EQ(lhs.aggressor_side, rhs.aggressor_side);
    EXPECT_TRUE(nearly_equal(lhs.price, rhs.price));
    EXPECT_EQ(lhs.size, rhs.size);
    EXPECT_EQ(lhs.trade_id, rhs.trade_id);
    EXPECT_EQ(to_millis(lhs.timestamp), to_millis(rhs.timestamp));
}

void expect_partial_fill_equal(const PartialFillEvent& lhs, const PartialFillEvent& rhs) {
    EXPECT_EQ(lhs.order_id, rhs.order_id);
    EXPECT_TRUE(nearly_equal(lhs.price, rhs.price));
    EXPECT_EQ(lhs.filled_size, rhs.filled_size);
    EXPECT_EQ(lhs.remaining_size, rhs.remaining_size);
    EXPECT_EQ(to_millis(lhs.timestamp), to_millis(rhs.timestamp));
}

void expect_event_equal(const MarketDataEvent& lhs, const MarketDataEvent& rhs) {
    EXPECT_EQ(lhs.instrument, rhs.instrument);
    EXPECT_TRUE(nearly_equal(lhs.best_bid_price, rhs.best_bid_price));
    EXPECT_TRUE(nearly_equal(lhs.best_ask_price, rhs.best_ask_price));
    EXPECT_EQ(lhs.best_bid_size, rhs.best_bid_size);
    EXPECT_EQ(lhs.best_ask_size, rhs.best_ask_size);
    EXPECT_EQ(lhs.sequence_number, rhs.sequence_number);
    EXPECT_EQ(to_millis(lhs.timestamp), to_millis(rhs.timestamp));

    ASSERT_EQ(lhs.bid_levels.size(), rhs.bid_levels.size());
    for (std::size_t i = 0; i < lhs.bid_levels.size(); ++i) {
        expect_order_level_equal(lhs.bid_levels[i], rhs.bid_levels[i]);
    }

    ASSERT_EQ(lhs.ask_levels.size(), rhs.ask_levels.size());
    for (std::size_t i = 0; i < lhs.ask_levels.size(); ++i) {
        expect_order_level_equal(lhs.ask_levels[i], rhs.ask_levels[i]);
    }

    ASSERT_EQ(lhs.trades.size(), rhs.trades.size());
    for (std::size_t i = 0; i < lhs.trades.size(); ++i) {
        expect_trade_equal(lhs.trades[i], rhs.trades[i]);
    }

    ASSERT_EQ(lhs.partial_fills.size(), rhs.partial_fills.size());
    for (std::size_t i = 0; i < lhs.partial_fills.size(); ++i) {
        expect_partial_fill_equal(lhs.partial_fills[i], rhs.partial_fills[i]);
    }
}

RunCapture run_capture(const SimulationConfig& config, int events_to_process) {
    MarketSimulator simulator(config);
    RunCapture run;
    double sum_bid = 0.0;
    double sum_ask = 0.0;

    for (int i = 0; i < events_to_process; ++i) {
        MarketDataEvent md;
        try {
            md = simulator.generate_event();
        } catch (const std::out_of_range&) {
            break;
        }

        run.events.push_back(md);
        ++run.digest.processed;
        sum_bid += md.best_bid_price;
        sum_ask += md.best_ask_price;
        run.digest.checksum = update_fnv1a(run.digest.checksum, event_fingerprint(md));
    }

    if (run.digest.processed > 0) {
        run.digest.avg_bid = sum_bid / run.digest.processed;
        run.digest.avg_ask = sum_ask / run.digest.processed;
    }
    return run;
}

SimulationConfig base_config() {
    SimulationConfig c;
    c.iterations = 200;
    return c;
}

}  // namespace

TEST(Determinism, SameSeedReproducible) {
    SimulationConfig a = base_config();
    a.seed = 12345;
    const RunCapture run_a = run_capture(a, a.iterations);
    EXPECT_EQ(run_a.digest.processed, a.iterations);

    SimulationConfig b = base_config();
    b.seed = 12345;
    const RunCapture run_b = run_capture(b, b.iterations);
    EXPECT_EQ(run_b.digest.processed, b.iterations);

    EXPECT_EQ(run_a.digest.checksum, run_b.digest.checksum);
    EXPECT_LT(std::abs(run_a.digest.avg_bid - run_b.digest.avg_bid), 1e-12);
    EXPECT_LT(std::abs(run_a.digest.avg_ask - run_b.digest.avg_ask), 1e-12);
}

TEST(Determinism, DifferentSeedDiverges) {
    SimulationConfig a = base_config();
    a.seed = 12345;
    const RunCapture run_a = run_capture(a, a.iterations);

    SimulationConfig c = base_config();
    c.seed = 54321;
    const RunCapture run_c = run_capture(c, c.iterations);
    EXPECT_EQ(run_c.digest.processed, c.iterations);
    EXPECT_NE(run_a.digest.checksum, run_c.digest.checksum);
}

TEST(Determinism, ReplayMatchesGeneration) {
    const std::string log_path =
        (std::filesystem::temp_directory_path() / "market_sim_determinism_replay.log").string();

    SimulationConfig writer = base_config();
    writer.seed = 777;
    writer.mode = SimulationMode::Simulate;
    writer.event_log_path = log_path;
    const RunCapture from_generation = run_capture(writer, writer.iterations);

    SimulationConfig replay = base_config();
    replay.seed = 999;
    replay.mode = SimulationMode::Replay;
    replay.replay_log_path = log_path;
    const RunCapture from_replay = run_capture(replay, replay.iterations);

    EXPECT_EQ(from_generation.digest.processed, from_replay.digest.processed);
    EXPECT_EQ(from_generation.digest.checksum, from_replay.digest.checksum);
    EXPECT_LT(std::abs(from_generation.digest.avg_bid - from_replay.digest.avg_bid), 1e-12);
    EXPECT_LT(std::abs(from_generation.digest.avg_ask - from_replay.digest.avg_ask), 1e-12);
    ASSERT_EQ(from_generation.events.size(), from_replay.events.size());
    for (std::size_t i = 0; i < from_generation.events.size(); ++i) {
        expect_event_equal(from_generation.events[i], from_replay.events[i]);
    }

    std::remove(log_path.c_str());
}
