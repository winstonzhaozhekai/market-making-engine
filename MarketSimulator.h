#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <random>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Order.h" // Include Order struct
#include "MarketDataEvent.h"

class MarketSimulator {
public:
    MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_);
    MarketDataEvent generate_event();

private:
    std::string instrument;
    double mid_price;
    double spread;
    double volatility;
    std::map<std::string, std::vector<Order>> order_book; // L2 order book
    std::mt19937 rng;

    void initialize_order_book();
    void update_order_book();
};

#endif // MARKET_SIMULATOR_H