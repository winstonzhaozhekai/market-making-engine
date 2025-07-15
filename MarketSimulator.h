#ifndef MARKET_SIMULATOR_H
#define MARKET_SIMULATOR_H

#include <string>
#include <random>
#include <chrono>

struct MarketDataEvent {
    std::string instrument;
    double bid;
    double ask;
    int bid_size;
    int ask_size;
    std::chrono::system_clock::time_point timestamp;
};

class MarketSimulator {
public:
    MarketSimulator(std::string instrument_, double init_price_, double spread_, double volatility_);
    MarketDataEvent generate_event();

private:
    std::string instrument;
    double mid_price;
    double spread;
    double volatility;
    std::mt19937 rng;
};

#endif // MARKET_SIMULATOR_H