#include "MarketSimulator.h"
#include "include/MdLogReader.h"
#include "include/SpscMdLogger.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace {
constexpr int64_t kBaseTimestampMs = 1700000000000LL;

constexpr uint64_t kSimOrderTag  = 2ULL << 48;
constexpr uint64_t kTradeIdTag   = 3ULL << 48;

std::chrono::system_clock::time_point from_millis(int64_t ms) {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
}
} // namespace

MarketSimulator::MarketSimulator(const SimulationConfig& cfg)
    : config(cfg),
      instrument_(cfg.tick_size),
      instrument(cfg.instrument),
      mid_price_dollars(cfg.initial_price),
      spread_dollars(cfg.spread),
      volatility(cfg.volatility),
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
        load_binary_event_log(config.replay_log_path);
        if (replay_events.empty()) {
            throw std::runtime_error("Replay log is empty: " + config.replay_log_path);
        }
        return;
    }

    if (!config.event_log_path.empty()) {
        event_logger_ = std::make_unique<SpscMdLogger>(config.event_log_path);
    }

    initialize_order_book();
}

MarketSimulator::~MarketSimulator() = default;

void MarketSimulator::initialize_order_book() {
    std::uniform_int_distribution<int> size_dist(1, 10);
    bid_levels_.reserve(5);
    ask_levels_.reserve(5);
    for (int i = 1; i <= 5; ++i) {
        double price_offset = i * spread_dollars / 2.0;
        Ticks bid_t = instrument_.to_ticks(mid_price_dollars - price_offset);
        Ticks ask_t = instrument_.to_ticks(mid_price_dollars + price_offset);
        bid_levels_.emplace_back(bid_t, size_dist(rng), generate_order_id(), current_time());
        ask_levels_.emplace_back(ask_t, size_dist(rng), generate_order_id(), current_time());
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
    mid_price_dollars += noise(rng);
    mid_price_dollars = std::max(mid_price_dollars, 0.01);

    update_order_book();

    trades_buf_.clear();
    mm_fills_buf_.clear();
    simulate_trade_activity(trades_buf_, mm_fills_buf_);

    auto event_creation_time = current_time();

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
        bid_levels_.empty() ? Ticks{0} : bid_levels_.front().price,
        ask_levels_.empty() ? Ticks{0} : ask_levels_.front().price,
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

    if (event_logger_) {
        event_logger_->log_event(event);
    }

    trades_buf_.reserve(4);
    mm_fills_buf_.reserve(8);

    return event;
}

void MarketSimulator::simulate_trade_activity(std::vector<Trade>& trades, std::vector<FillEvent>& mm_fills) {
    std::uniform_real_distribution<> prob_dist(0.0, 1.0);
    std::uniform_int_distribution<> size_dist(1, 20);

    if (prob_dist(rng) < 0.2) {
        bool is_buy = prob_dist(rng) < 0.5;
        Side aggressor_side = is_buy ? Side::BUY : Side::SELL;
        auto& levels = is_buy ? ask_levels_ : bid_levels_;

        if (!levels.empty()) {
            int trade_size = size_dist(rng);
            Ticks trade_price = levels[0].price;
            uint64_t trade_id = kTradeIdTag | static_cast<uint64_t>(sequence_number + 1);
            auto ts = current_time();

            trades.emplace_back(Trade{
                aggressor_side,
                trade_price,
                trade_size,
                trade_id,
                ts
            });

            // Aggressor flows through the same add_order entry-point as any
            // other order — with IOC semantics, residual is discarded
            // rather than rested (matches the "random market sweep"
            // intent and unifies the engine API around one path).
            Order aggressor(kTradeIdTag | trade_id, aggressor_side,
                            trade_price, trade_size, ts);
            auto fills = matching_engine.add_order(
                std::move(aggressor), OrderType::IOC, trade_id).fills;
            mm_fills.insert(mm_fills.end(), fills.begin(), fills.end());
        }
    }
}

OrderStatus MarketSimulator::submit_order(const Order& order, OrderType type) {
    return matching_engine.add_order(order, type).status;
}

bool MarketSimulator::cancel_order(uint64_t order_id) {
    return matching_engine.cancel_order(order_id);
}

bool MarketSimulator::amend_order(uint64_t order_id, Ticks new_price,
                                  int new_qty,
                                  std::chrono::system_clock::time_point ts) {
    return matching_engine.amend_order(order_id, new_price, new_qty, ts);
}

void MarketSimulator::update_order_book() {
    std::uniform_real_distribution<> noise_dist(-0.001, 0.001);
    std::uniform_int_distribution<> size_change_dist(-2, 2);

    // Re-anchor each level around the (sub-tick) mid in dollars, then snap.
    // Without this, bid/ask levels drift far from mid_price, giving the
    // strategy stale market data and a permanently zero sigma estimate.
    for (std::size_t i = 0; i < bid_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread_dollars / 2.0;
        bid_levels_[i].price = instrument_.to_ticks(
            mid_price_dollars - base_offset + noise_dist(rng));
        bid_levels_[i].size = std::max(1, bid_levels_[i].size + size_change_dist(rng));
    }
    std::sort(bid_levels_.begin(), bid_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price > b.price; });

    for (std::size_t i = 0; i < ask_levels_.size(); ++i) {
        double base_offset = static_cast<double>(i + 1) * spread_dollars / 2.0;
        ask_levels_[i].price = instrument_.to_ticks(
            mid_price_dollars + base_offset + noise_dist(rng));
        ask_levels_[i].size = std::max(1, ask_levels_[i].size + size_change_dist(rng));
    }
    std::sort(ask_levels_.begin(), ask_levels_.end(),
              [](const OrderLevel& a, const OrderLevel& b) { return a.price < b.price; });
}

uint64_t MarketSimulator::generate_order_id() {
    return kSimOrderTag | ++sim_order_counter_;
}

// `simulation_clock` is a deterministic, monotonic synthetic clock — NOT real
// wall-clock time. It is initialized from `cfg.seed` and advances by exactly
// 1 ms on every call. Each `generate_event()` calls this multiple times (once
// per built timestamp), so a single market-data event spans several "ms" of
// simulator time. Implications:
//   - Same-seed runs produce byte-identical timestamps (replay determinism).
//   - Different seeds produce different absolute timestamps; absolute values
//     are not comparable across seeds.
//   - Risk rate-windows and stale-data checks are evaluated against this
//     synthetic clock, so their semantics are tied to *call frequency*, not
//     real elapsed time.
//   - This type uses `system_clock::time_point` only as a typed monotonic
//     counter. NTP slewing cannot affect it because `system_clock::now()` is
//     never called for these values. The type will be migrated when per-stage
//     latency lands (M8).
std::chrono::system_clock::time_point MarketSimulator::current_time() {
    simulation_clock += std::chrono::milliseconds(1);
    return simulation_clock;
}

void MarketSimulator::load_binary_event_log(const std::string& path) {
    MdLogReader reader(path);
    while (auto ev = reader.next()) {
        replay_events.push_back(std::move(*ev));
    }
    if (!replay_events.empty()) {
        sequence_number = replay_events.back().sequence_number;
        simulation_clock = replay_events.back().timestamp;
    }
}
