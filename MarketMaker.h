#ifndef MARKET_MAKER_H
#define MARKET_MAKER_H

#include "MarketSimulator.h"
#include <map>
#include <vector>
#include <string>

struct OrderBookLevel {
    double price;
    int size;
};

class MarketMaker {
public:
    void on_market_data(const MarketDataEvent& md);
    void report() const;
    void process_trades(const std::vector<Trade>& trades); // Add process_trades method

private:
    void trade(const MarketDataEvent& md);

    std::map<std::string, OrderBookLevel> order_book;
    std::vector<MarketDataEvent> market_data_log;
    int inventory = 0;
    double cash = 0.0;
};

#endif // MARKET_MAKER_H