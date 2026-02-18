#include "MarketSimulator.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
constexpr int64_t kBaseTimestampMs = 1700000000000LL;

// Packed ID tags
constexpr uint64_t kSimOrderTag  = 2ULL << 48;
constexpr uint64_t kTradeIdTag   = 3ULL << 48;

std::vector<std::string> split(const std::string& input, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    std::size_t pos = input.find(delimiter);
    while (pos != std::string::npos) {
        out.push_back(input.substr(start, pos - start));
        start = pos + 1;
        pos = input.find(delimiter, start);
    }
    out.push_back(input.substr(start));
    return out;
}

int64_t to_millis(const std::chrono::system_clock::time_point& ts) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(ts.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_millis(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}

const char* side_to_str(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

Side str_to_side(const std::string& s) {
    return s == "BUY" ? Side::BUY : Side::SELL;
}
} // namespace

MarketSimulator::MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_)
    : MarketSimulator(SimulationConfig{
          std::move(instrument_),
          init_price_,
          spread_,
          volatility_,
          latency_ms_,
          1000,
          42,
          "",
          "",
          SimulationMode::Simulate,
          false}) {}

MarketSimulator::MarketSimulator(const SimulationConfig& cfg)
    : config(cfg),
      instrument(cfg.instrument),
      mid_price(cfg.initial_price),
      spread(cfg.spread),
      volatility(cfg.volatility),
      latency_ms(cfg.latency_ms),
      rng(cfg.seed),
      sequence_number(0),
      simulation_clock(from_millis(kBaseTimestampMs + static_cast<int64_t>(cfg.seed) * 1000)),
      replay_index(0) {

    trades_buf_.reserve(4);
    mm_fills_buf_.reserve(8);

    if (config.mode == SimulationMode::Replay) {
        if (config.replay_log_path.empty()) {
            throw std::runtime_error("Replay mode requires a replay log path");
        }
        if (!load_event_log(config.replay_log_path)) {
            throw std::runtime_error("Failed to load replay log: " + config.replay_log_path);
        }
        if (replay_events.empty()) {
            throw std::runtime_error("Replay log is empty: " + config.replay_log_path);
        }
        return;
    }

    if (!config.event_log_path.empty()) {
        event_log_stream.open(config.event_log_path, std::ios::out | std::ios::trunc);
        if (!event_log_stream) {
            throw std::runtime_error("Failed to open event log for writing: " + config.event_log_path);
        }
    }

    initialize_order_book();
}

void MarketSimulator::initialize_order_book() {
    std::uniform_int_distribution<int> size_dist(1, 10);
    bid_levels_.reserve(5);
    ask_levels_.reserve(5);
    for (int i = 1; i <= 5; ++i) {
        double price_offset = i * spread / 2;
        bid_levels_.emplace_back(mid_price - price_offset, size_dist(rng), generate_order_id(), current_time());
        ask_levels_.emplace_back(mid_price + price_offset, size_dist(rng), generate_order_id(), current_time());
    }
}

MarketDataEvent MarketSimulator::generate_event() {
    if (!replay_events.empty()) {
        if (replay_index >= replay_events.size()) {
            throw std::out_of_range("Replay log exhausted");
        }
        return replay_events[replay_index++];
    }

    std::normal_distribution<> noise(0, volatility);
    mid_price += noise(rng);
    mid_price = std::max(mid_price, 0.01);

    update_order_book();

    // Reuse pre-allocated buffers
    trades_buf_.clear();
    mm_fills_buf_.clear();
    simulate_trade_activity(trades_buf_, mm_fills_buf_);

    auto event_creation_time = current_time();
    if (latency_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(latency_ms));
    }

    // Build partial_fills from mm_fills for backwards compatibility
    std::vector<PartialFillEvent> partial_fills;
    for (const auto& fill : mm_fills_buf_) {
        if (fill.leaves_qty > 0) {
            partial_fills.push_back(PartialFillEvent{
                fill.order_id,
                fill.price,
                fill.fill_qty,
                fill.leaves_qty,
                fill.timestamp
            });
        }
    }

    MarketDataEvent event{
        instrument,
        bid_levels_.empty() ? 0.0 : bid_levels_.front().price,
        ask_levels_.empty() ? 0.0 : ask_levels_.front().price,
        bid_levels_.empty() ? 0 : bid_levels_.front().size,
        ask_levels_.empty() ? 0 : ask_levels_.front().size,
        bid_levels_,
        ask_levels_,
        std::move(trades_buf_),
        std::move(partial_fills),
        std::move(mm_fills_buf_),
        event_creation_time,
        ++sequence_number
    };

    maybe_write_event_log(event);

    // Re-initialize moved-from buffers for next event
    trades_buf_.reserve(4);
    mm_fills_buf_.reserve(8);

    return event;
}

void MarketSimulator::simulate_trade_activity(std::vector<Trade>& trades, std::vector<FillEvent>& mm_fills) {
    std::uniform_real_distribution<> prob_dist(0.0, 1.0);
    std::uniform_int_distribution<> size_dist(1, 20);

    // 20% chance of trade activity
    if (prob_dist(rng) < 0.2) {
        bool is_buy = prob_dist(rng) < 0.5;
        Side aggressor_side = is_buy ? Side::BUY : Side::SELL;
        auto& levels = is_buy ? ask_levels_ : bid_levels_;

        if (!levels.empty()) {
            int trade_size = size_dist(rng);
            double trade_price = levels[0].price;
            uint64_t trade_id = kTradeIdTag | static_cast<uint64_t>(sequence_number + 1);
            auto ts = current_time();

            trades.emplace_back(Trade{
                aggressor_side,
                trade_price,
                trade_size,
                trade_id,
                ts
            });

            // Route through matching engine to fill MM resting orders
            auto fills = matching_engine.match_incoming_order(
                aggressor_side, trade_price, trade_size, trade_id, ts);
            mm_fills.insert(mm_fills.end(), fills.begin(), fills.end());
        }
    }
}

OrderStatus MarketSimulator::submit_order(const Order& order) {
    return matching_engine.add_order(order);
}

bool MarketSimulator::cancel_order(uint64_t order_id) {
    return matching_engine.cancel_order(order_id);
}

void MarketSimulator::update_order_book() {
    std::uniform_real_distribution<> noise_dist(-0.001, 0.001);
    std::uniform_int_distribution<> size_change_dist(-2, 2);

    // Re-anchor each level around mid_price so the book tracks actual price movements.
    // Without this, bid/ask levels drift far from mid_price, giving the strategy
    // stale market data and a permanently zero sigma estimate.
    for (std::size_t i = 0; i < bid_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread / 2.0;
        bid_levels_[i].price = mid_price - base_offset + noise_dist(rng);
        bid_levels_[i].size = std::max(1, bid_levels_[i].size + size_change_dist(rng));
    }
    std::sort(bid_levels_.begin(), bid_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price > b.price; });

    for (std::size_t i = 0; i < ask_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread / 2.0;
        ask_levels_[i].price = mid_price + base_offset + noise_dist(rng);
        ask_levels_[i].size = std::max(1, ask_levels_[i].size + size_change_dist(rng));
    }
    std::sort(ask_levels_.begin(), ask_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price < b.price; });
}

uint64_t MarketSimulator::generate_order_id() {
    return kSimOrderTag | ++sim_order_counter_;
}

std::chrono::system_clock::time_point MarketSimulator::current_time() {
    simulation_clock += std::chrono::milliseconds(1);
    return simulation_clock;
}

void MarketSimulator::maybe_write_event_log(const MarketDataEvent& event) {
    if (!event_log_stream) {
        return;
    }
    event_log_stream << serialize_event(event) << "\n";
}

std::string MarketSimulator::serialize_event(const MarketDataEvent& event) {
    auto serialize_levels = [](const std::vector<OrderLevel>& levels) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (std::size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            oss << level.price << "," << level.size << "," << level.order_id << "," << to_millis(level.timestamp);
            if (i + 1 < levels.size()) {
                oss << ";";
            }
        }
        return oss.str();
    };

    auto serialize_trades = [](const std::vector<Trade>& trades) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (std::size_t i = 0; i < trades.size(); ++i) {
            const auto& trade = trades[i];
            oss << side_to_str(trade.aggressor_side) << "," << trade.price << "," << trade.size << "," << trade.trade_id << "," << to_millis(trade.timestamp);
            if (i + 1 < trades.size()) {
                oss << ";";
            }
        }
        return oss.str();
    };

    auto serialize_partial_fills = [](const std::vector<PartialFillEvent>& fills) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::max_digits10);
        for (std::size_t i = 0; i < fills.size(); ++i) {
            const auto& fill = fills[i];
            oss << fill.order_id << "," << fill.price << "," << fill.filled_size << "," << fill.remaining_size << "," << to_millis(fill.timestamp);
            if (i + 1 < fills.size()) {
                oss << ";";
            }
        }
        return oss.str();
    };

    std::ostringstream line;
    line << std::setprecision(std::numeric_limits<double>::max_digits10);
    line << event.sequence_number << "|"
         << event.instrument << "|"
         << event.best_bid_price << "|"
         << event.best_ask_price << "|"
         << event.best_bid_size << "|"
         << event.best_ask_size << "|"
         << to_millis(event.timestamp) << "|"
         << serialize_levels(event.bid_levels) << "|"
         << serialize_levels(event.ask_levels) << "|"
         << serialize_trades(event.trades) << "|"
         << serialize_partial_fills(event.partial_fills);
    return line.str();
}

MarketDataEvent MarketSimulator::deserialize_event(const std::string& line) {
    const auto fields = split(line, '|');
    if (fields.size() != 11) {
        throw std::runtime_error("Malformed replay log line");
    }

    auto parse_levels = [](const std::string& raw) {
        std::vector<OrderLevel> levels;
        if (raw.empty()) {
            return levels;
        }
        for (const auto& entry : split(raw, ';')) {
            if (entry.empty()) {
                continue;
            }
            const auto tokens = split(entry, ',');
            if (tokens.size() != 4) {
                throw std::runtime_error("Malformed level entry");
            }
            levels.emplace_back(
                std::stod(tokens[0]),
                std::stoi(tokens[1]),
                std::stoull(tokens[2]),
                from_millis(std::stoll(tokens[3])));
        }
        return levels;
    };

    auto parse_trades = [](const std::string& raw) {
        std::vector<Trade> trades;
        if (raw.empty()) {
            return trades;
        }
        for (const auto& entry : split(raw, ';')) {
            if (entry.empty()) {
                continue;
            }
            const auto tokens = split(entry, ',');
            if (tokens.size() != 5) {
                throw std::runtime_error("Malformed trade entry");
            }
            trades.push_back(Trade{
                str_to_side(tokens[0]),
                std::stod(tokens[1]),
                std::stoi(tokens[2]),
                std::stoull(tokens[3]),
                from_millis(std::stoll(tokens[4]))});
        }
        return trades;
    };

    auto parse_partial_fills = [](const std::string& raw) {
        std::vector<PartialFillEvent> fills;
        if (raw.empty()) {
            return fills;
        }
        for (const auto& entry : split(raw, ';')) {
            if (entry.empty()) {
                continue;
            }
            const auto tokens = split(entry, ',');
            if (tokens.size() != 5) {
                throw std::runtime_error("Malformed partial fill entry");
            }
            fills.push_back(PartialFillEvent{
                std::stoull(tokens[0]),
                std::stod(tokens[1]),
                std::stoi(tokens[2]),
                std::stoi(tokens[3]),
                from_millis(std::stoll(tokens[4]))});
        }
        return fills;
    };

    MarketDataEvent event;
    event.instrument = fields[1];
    event.best_bid_price = std::stod(fields[2]);
    event.best_ask_price = std::stod(fields[3]);
    event.best_bid_size = std::stoi(fields[4]);
    event.best_ask_size = std::stoi(fields[5]);
    event.timestamp = from_millis(std::stoll(fields[6]));
    event.bid_levels = parse_levels(fields[7]);
    event.ask_levels = parse_levels(fields[8]);
    event.trades = parse_trades(fields[9]);
    event.partial_fills = parse_partial_fills(fields[10]);
    event.sequence_number = std::stoll(fields[0]);
    return event;
}

bool MarketSimulator::load_event_log(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        replay_events.push_back(deserialize_event(line));
    }

    if (!replay_events.empty()) {
        sequence_number = replay_events.back().sequence_number;
        simulation_clock = replay_events.back().timestamp;
    }
    return true;
}
