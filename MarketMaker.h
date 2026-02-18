#ifndef MARKETMAKER_H
#define MARKETMAKER_H

#include "MarketDataEvent.h"
#include "Order.h"
#include "include/Accounting.h"
#include "include/RiskManager.h"
#include "include/Strategy.h"
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdint>
#include <memory>

class MarketSimulator;

class MarketMaker {
public:
    MarketMaker();
    explicit MarketMaker(const RiskConfig& cfg);
    MarketMaker(const RiskConfig& cfg, std::unique_ptr<Strategy> strategy);
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
    double get_drawdown() const;
    double get_high_water_mark() const;
    const char* get_strategy_name() const;
    RiskState get_risk_state() const;
    const std::vector<RiskRuleResult>& get_risk_details() const;

private:
    std::unordered_map<uint64_t, Order> active_orders;
    double last_bid_price_ = 0.0;
    double last_ask_price_ = 0.0;
    bool has_last_event_ = false;
    Accounting accounting_{100000.0};
    RiskManager risk_manager_;
    std::unique_ptr<Strategy> strategy_;
    std::chrono::system_clock::time_point last_quote_time;
    int64_t last_processed_sequence = 0;
    int order_counter = 0;
    int total_fills = 0;

    void on_fill(const FillEvent& fill);
    void update_quotes(const MarketDataEvent& md, MarketSimulator& simulator);
    void cancel_all_orders(MarketSimulator& simulator, std::chrono::system_clock::time_point now);
    uint64_t generate_order_id();
};

#endif
