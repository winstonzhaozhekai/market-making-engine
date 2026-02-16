#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
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
        fp << "|T:" << trade.aggressor_side << ":"
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

void assert_order_level_equal(const OrderLevel& lhs, const OrderLevel& rhs) {
    assert(nearly_equal(lhs.price, rhs.price));
    assert(lhs.size == rhs.size);
    assert(lhs.order_id == rhs.order_id);
    assert(to_millis(lhs.timestamp) == to_millis(rhs.timestamp));
}

void assert_trade_equal(const Trade& lhs, const Trade& rhs) {
    assert(lhs.aggressor_side == rhs.aggressor_side);
    assert(nearly_equal(lhs.price, rhs.price));
    assert(lhs.size == rhs.size);
    assert(lhs.trade_id == rhs.trade_id);
    assert(to_millis(lhs.timestamp) == to_millis(rhs.timestamp));
}

void assert_partial_fill_equal(const PartialFillEvent& lhs, const PartialFillEvent& rhs) {
    assert(lhs.order_id == rhs.order_id);
    assert(nearly_equal(lhs.price, rhs.price));
    assert(lhs.filled_size == rhs.filled_size);
    assert(lhs.remaining_size == rhs.remaining_size);
    assert(to_millis(lhs.timestamp) == to_millis(rhs.timestamp));
}

void assert_event_equal(const MarketDataEvent& lhs, const MarketDataEvent& rhs) {
    assert(lhs.instrument == rhs.instrument);
    assert(nearly_equal(lhs.best_bid_price, rhs.best_bid_price));
    assert(nearly_equal(lhs.best_ask_price, rhs.best_ask_price));
    assert(lhs.best_bid_size == rhs.best_bid_size);
    assert(lhs.best_ask_size == rhs.best_ask_size);
    assert(lhs.sequence_number == rhs.sequence_number);
    assert(to_millis(lhs.timestamp) == to_millis(rhs.timestamp));

    assert(lhs.bid_levels.size() == rhs.bid_levels.size());
    for (std::size_t i = 0; i < lhs.bid_levels.size(); ++i) {
        assert_order_level_equal(lhs.bid_levels[i], rhs.bid_levels[i]);
    }

    assert(lhs.ask_levels.size() == rhs.ask_levels.size());
    for (std::size_t i = 0; i < lhs.ask_levels.size(); ++i) {
        assert_order_level_equal(lhs.ask_levels[i], rhs.ask_levels[i]);
    }

    assert(lhs.trades.size() == rhs.trades.size());
    for (std::size_t i = 0; i < lhs.trades.size(); ++i) {
        assert_trade_equal(lhs.trades[i], rhs.trades[i]);
    }

    assert(lhs.partial_fills.size() == rhs.partial_fills.size());
    for (std::size_t i = 0; i < lhs.partial_fills.size(); ++i) {
        assert_partial_fill_equal(lhs.partial_fills[i], rhs.partial_fills[i]);
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
} // namespace

int main() {
    SimulationConfig base;
    base.iterations = 200;
    base.latency_ms = 0;

    SimulationConfig same_seed_a = base;
    same_seed_a.seed = 12345;
    const RunCapture run_a = run_capture(same_seed_a, same_seed_a.iterations);
    assert(run_a.digest.processed == same_seed_a.iterations);

    SimulationConfig same_seed_b = base;
    same_seed_b.seed = 12345;
    const RunCapture run_b = run_capture(same_seed_b, same_seed_b.iterations);
    assert(run_b.digest.processed == same_seed_b.iterations);
    assert(run_a.digest.checksum == run_b.digest.checksum);
    assert(std::abs(run_a.digest.avg_bid - run_b.digest.avg_bid) < 1e-12);
    assert(std::abs(run_a.digest.avg_ask - run_b.digest.avg_ask) < 1e-12);

    SimulationConfig different_seed = base;
    different_seed.seed = 54321;
    const RunCapture run_c = run_capture(different_seed, different_seed.iterations);
    assert(run_c.digest.processed == different_seed.iterations);
    assert(run_a.digest.checksum != run_c.digest.checksum);

    const std::string log_path = "/tmp/market_sim_determinism_replay.log";
    SimulationConfig writer = base;
    writer.seed = 777;
    writer.mode = SimulationMode::Simulate;
    writer.event_log_path = log_path;
    const RunCapture from_generation = run_capture(writer, writer.iterations);

    SimulationConfig replay = base;
    replay.seed = 999;
    replay.mode = SimulationMode::Replay;
    replay.replay_log_path = log_path;
    const RunCapture from_replay = run_capture(replay, replay.iterations);
    assert(from_generation.digest.processed == from_replay.digest.processed);
    assert(from_generation.digest.checksum == from_replay.digest.checksum);
    assert(std::abs(from_generation.digest.avg_bid - from_replay.digest.avg_bid) < 1e-12);
    assert(std::abs(from_generation.digest.avg_ask - from_replay.digest.avg_ask) < 1e-12);
    assert(from_generation.events.size() == from_replay.events.size());
    for (std::size_t i = 0; i < from_generation.events.size(); ++i) {
        assert_event_equal(from_generation.events[i], from_replay.events[i]);
    }

    std::remove(log_path.c_str());

    std::cout << "Determinism tests passed: "
              << "same-seed stable, different-seed diverges, replay matches generation byte-for-byte.\n";
    return 0;
}
