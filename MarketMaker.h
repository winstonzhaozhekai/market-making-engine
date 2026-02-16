#ifndef MARKETMAKER_H
#define MARKETMAKER_H

#include "MarketDataEvent.h"
#include "Order.h"
#include "include/Accounting.h"
#include <map>
#include <vector>
#include <chrono>
#include <string>

class MarketSimulator;

struct RiskLimits {
    int max_position = 1000;
    int max_daily_loss = 10000;
    double max_quote_spread = 0.5;
    int min_quote_size = 1;
    int max_quote_size = 100;
};

class MarketMaker {
public:
    MarketMaker();
    void on_market_data(const MarketDataEvent& md, MarketSimulator& simulator);
    void report();
    double get_cash() const;
    int get_inventory() const;
    double get_mark_price() const;
    double get_unrealized_pnl() const;
    double get_realized_pnl() const;
    double get_total_pnl() const;
    int get_total_fills() const;
    double get_inventory_skew() const;
    double get_fees() const;
    double get_rebates() const;
    double get_avg_entry_price() const;
    double get_gross_exposure() const;
    double get_net_exposure() const;

private:
    std::map<std::string, Order> active_orders;
    std::vector<MarketDataEvent> market_data_log;
    Accounting accounting_{100000.0};
    RiskLimits risk_limits;
    std::chrono::system_clock::time_point last_quote_time;
    int64_t last_processed_sequence = 0;
    int order_counter = 0;
    int total_fills = 0;

    bool check_risk_limits(const MarketDataEvent& md);
    void on_fill(const FillEvent& fill);
    void update_quotes(const MarketDataEvent& md, MarketSimulator& simulator);
    void cancel_all_orders(MarketSimulator& simulator);
    int calculate_optimal_quote_size(const MarketDataEvent& md, Side side);
    double calculate_inventory_skew() const;
    std::string generate_order_id();
};

#endif
