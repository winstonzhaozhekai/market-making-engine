#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <random>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Order.h" // Include Order struct

struct Trade {
    double price;
    int size;
    std::string aggressor_side;
    std::chrono::system_clock::time_point timestamp;

    // Constructor to match usage in MarketSimulator
    Trade(double price_, int size_, std::string aggressor_side_, std::chrono::system_clock::time_point timestamp_)
        : price(price_), size(size_), aggressor_side(std::move(aggressor_side_)), timestamp(timestamp_) {}
};

struct MarketDataEvent {
    std::string instrument;
    double best_bid_price;
    double best_ask_price;
    int best_bid_size;
    int best_ask_size;
    std::vector<Order> bid_levels;
    std::vector<Order> ask_levels;
    std::chrono::system_clock::time_point timestamp;
    std::vector<Trade> trades; // Add trades to market data
};

class MarketSimulator {
public:
    MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_, int latency_ms_);
    MarketDataEvent generate_event();

private:
    std::string instrument;
    double mid_price;
    double spread;
    double volatility;
    int latency_ms; // Add latency_ms member
    std::map<std::string, std::vector<Order>> order_book; // L2 order book
    std::mt19937 rng;

    void initialize_order_book();
    void update_order_book();
    void simulate_trade(); // Add simulate_trade method
    std::string generate_order_id(); // Add generate_order_id method
    std::chrono::system_clock::time_point current_time(); // Add current_time method
    std::vector<Trade> trades; // Add trades member
};

#endif // MARKET_SIMULATOR_H