#ifndef MARKETMAKER_H
#define MARKETMAKER_H

#include "MarketDataEvent.h"
#include <map>
#include <vector>
#include <chrono>

struct Quote {
    double price;
    int size;
    std::string order_id;
    std::chrono::system_clock::time_point timestamp;
};

struct RiskLimits {
    int max_position = 1000;
    int max_daily_loss = 10000;
    double max_quote_spread = 0.5;
    int min_quote_size = 1;
    int max_quote_size = 100;
};

class MarketMaker {
private:
    std::map<std::string, Quote> order_book;
    std::vector<MarketDataEvent> market_data_log;
    double cash = 100000.0;
    int inventory = 0;
    RiskLimits risk_limits;
    double daily_pnl = 0.0;
    std::chrono::system_clock::time_point last_quote_time;
    int64_t last_processed_sequence = 0;
    
    // Order management
    std::map<std::string, Quote> active_orders;
    int order_counter = 0;
    
    // Performance tracking
    double total_slippage = 0.0;
    int missed_opportunities = 0;
    
public:
    MarketMaker();
    void on_market_data(const MarketDataEvent& md);
    void report();
    
private:
    bool check_risk_limits(const MarketDataEvent& md);
    void process_trades(const std::vector<Trade>& trades);
    void process_partial_fills(const std::vector<PartialFillEvent>& fills);
    void update_quotes(const MarketDataEvent& md);
    double calculate_slippage(double price, int size, const std::string& side);
    int calculate_optimal_quote_size(const MarketDataEvent& md, const std::string& side);
    double calculate_inventory_skew();
    std::string generate_order_id();
    bool is_stale_quote(const std::chrono::system_clock::time_point& quote_time);
};

#endif