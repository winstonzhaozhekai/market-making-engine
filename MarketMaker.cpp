#include "MarketMaker.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>

void MarketMaker::on_market_data(const MarketDataEvent& md) {
    double bid = md.bid + 0.01; // skew toward inside market
    double ask = md.ask - 0.01;
    order_book["BID"] = {bid, 5};
    order_book["ASK"] = {ask, 5};
    market_data_log.push_back(md);
    trade(md);
}

void MarketMaker::trade(const MarketDataEvent& md) {
    if ((rand() % 100) < 5) {  // 5% chance of hitting bid
        inventory += order_book["BID"].size;
        cash -= order_book["BID"].price * order_book["BID"].size;
    }
    if ((rand() % 100) < 5) {  // 5% chance of lifting ask
        inventory -= order_book["ASK"].size;
        cash += order_book["ASK"].price * order_book["ASK"].size;
    }
}

void MarketMaker::report() const {
    double mark = market_data_log.back().bid + (market_data_log.back().ask - market_data_log.back().bid) / 2;
    double pnl = cash + inventory * mark;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Inventory: " << inventory << ", PnL: $" << pnl << std::endl;
}