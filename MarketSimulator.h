#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <random>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "MarketDataEvent.h" // Include the correct header file

class MarketSimulator {
public:
    MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_);
    MarketDataEvent generate_event();

private:
    std::string instrument;
    double mid_price;
    double spread;
    double volatility;
    int latency_ms;
    std::map<std::string, std::vector<OrderLevel>> order_book; // Use OrderLevel from MarketDataEvent.h
    std::mt19937 rng;
    int64_t sequence_number; // Declare sequence_number

    void initialize_order_book();
    void update_order_book();
    void simulate_trade_activity(std::vector<Trade>& trades, std::vector<PartialFillEvent>& partial_fills);
    std::string generate_order_id();
    std::chrono::system_clock::time_point current_time();
};

#endif // MARKET_SIMULATOR_H