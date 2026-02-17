#ifndef STRATEGY_H
#define STRATEGY_H

#include "../MarketDataEvent.h"
#include <vector>
#include <chrono>

struct StrategySnapshot {
    double best_bid = 0.0;
    double best_ask = 0.0;
    double mid_price = 0.0;
    std::vector<OrderLevel> bid_levels;
    std::vector<OrderLevel> ask_levels;
    std::vector<Trade> trades;
    int position = 0;
    int max_position = 1000;
    std::chrono::system_clock::time_point timestamp;
    int64_t sequence_number = 0;
};

struct QuoteDecision {
    double bid_price = 0.0;
    double ask_price = 0.0;
    int bid_size = 0;
    int ask_size = 0;
    bool should_quote = true;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual QuoteDecision compute_quotes(const StrategySnapshot& snapshot) = 0;
    virtual const char* name() const = 0;
};

#endif // STRATEGY_H
