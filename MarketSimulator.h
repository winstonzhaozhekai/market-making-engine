#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <chrono>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "MarketDataEvent.h"
#include "MatchingEngine.h"
#include "include/SimulationConfig.h"

class MarketSimulator {
public:
    MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_);
    explicit MarketSimulator(const SimulationConfig& config);
    MarketDataEvent generate_event();

    // MM order submission interface
    OrderStatus submit_order(const Order& order);
    bool cancel_order(uint64_t order_id);
    const MatchingEngine& get_matching_engine() const { return matching_engine; }

private:
    SimulationConfig config;
    std::string instrument;
    double mid_price;
    double spread;
    double volatility;
    int latency_ms;
    std::vector<OrderLevel> bid_levels_;
    std::vector<OrderLevel> ask_levels_;
    MatchingEngine matching_engine;
    std::mt19937 rng;
    int64_t sequence_number;
    uint64_t sim_order_counter_ = 0;
    std::chrono::system_clock::time_point simulation_clock;
    std::ofstream event_log_stream;
    std::vector<MarketDataEvent> replay_events;
    std::size_t replay_index;

    // Pre-allocated vectors reused across events
    std::vector<Trade> trades_buf_;
    std::vector<FillEvent> mm_fills_buf_;

    void initialize_order_book();
    void update_order_book();
    void simulate_trade_activity(std::vector<Trade>& trades, std::vector<FillEvent>& mm_fills);
    uint64_t generate_order_id();
    std::chrono::system_clock::time_point current_time();
    void maybe_write_event_log(const MarketDataEvent& event);
    bool load_event_log(const std::string& path);
    static std::string serialize_event(const MarketDataEvent& event);
    static MarketDataEvent deserialize_event(const std::string& line);
};

#endif // MARKET_SIMULATOR_H
