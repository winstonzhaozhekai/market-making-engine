#include "MarketMaker.h"
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <numeric>

void MarketMaker::on_market_data(const MarketDataEvent& md) {
    const int min_volume_threshold = 10; // Minimum volume threshold for quoting
    const double spread = 0.02; // Fixed spread for quoting

    // Check if there is sufficient depth in the order book
    int bid_volume = 0;
    int ask_volume = 0;
    for (const auto& level : md.bid_levels) {
        bid_volume += level.size;
    }
    for (const auto& level : md.ask_levels) {
        ask_volume += level.size;
    }

    // Skip quoting if best bid/ask volume is too low
    if (bid_volume < min_volume_threshold || ask_volume < min_volume_threshold) {
        return;
    }

    // Calculate mid-price
    if (md.bid_levels.empty() || md.ask_levels.empty()) {
        return; // Skip if there is no bid or ask data
    }
    double best_bid = md.bid_levels[0].price;
    double best_ask = md.ask_levels[0].price;
    double mid_price = (best_bid + best_ask) / 2;

    // Adjust quotes based on mid-price and fixed spread
    double bid = mid_price - spread / 2;
    double ask = mid_price + spread / 2;

    order_book["BID"] = {bid, 5};
    order_book["ASK"] = {ask, 5};
    market_data_log.push_back(md);
    trade(md);

    // Process trades from MarketSimulator
    process_trades(md.trades); // Ensure MarketDataEvent includes trades
}

void MarketMaker::trade(const MarketDataEvent& md) {
    std::cout << "Quote: BID " << order_book["BID"].price 
              << " | ASK " << order_book["ASK"].price << "\n";
    std::cout << "Market: Best Bid " << md.bid_levels[0].price 
              << " | Best Ask " << md.ask_levels[0].price << "\n";

    // Simulate a market event: either buy or sell side arrives
    enum class Side { BUY, SELL };
    Side incoming = (rand() % 2 == 0) ? Side::BUY : Side::SELL;

    if (incoming == Side::BUY) {
        // Someone buys from us => check if market bid crosses our ask
        if (!md.bid_levels.empty() && md.bid_levels[0].price >= order_book["ASK"].price) {
            inventory -= order_book["ASK"].size;
            cash += order_book["ASK"].price * order_book["ASK"].size;
            std::cout << "ASK filled (market BUY)\n";
        }
    } else {
        // Someone sells to us => check if market ask crosses our bid
        if (!md.ask_levels.empty() && md.ask_levels[0].price <= order_book["BID"].price) {
            inventory += order_book["BID"].size;
            cash -= order_book["BID"].price * order_book["BID"].size;
            std::cout << "BID filled (market SELL)\n";
        }
    }
}

void MarketMaker::report() const {
    double mark = market_data_log.back().best_bid_price + 
                  (market_data_log.back().best_ask_price - market_data_log.back().best_bid_price) / 2;
    double pnl = cash + inventory * mark;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Inventory: " << inventory << ", PnL: $" << pnl << std::endl;
}

void MarketMaker::process_trades(const std::vector<Trade>& trades) {
    for (const auto& trade : trades) {
        if (trade.aggressor_side == "BUY") {
            inventory -= trade.size;
            cash += trade.price * trade.size;
        } else if (trade.aggressor_side == "SELL") {
            inventory += trade.size;
            cash -= trade.price * trade.size;
        }
        std::cout << "Processed trade: " << trade.aggressor_side 
                  << " | Price: " << trade.price 
                  << " | Size: " << trade.size << std::endl;
    }
}