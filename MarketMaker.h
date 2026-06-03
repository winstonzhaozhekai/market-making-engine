#ifndef MARKETMAKER_H
#define MARKETMAKER_H

#include "MarketDataEvent.h"
#include "Order.h"
#include "include/Accounting.h"
#include "include/Instrument.h"
#include "include/Logger.h"
#include "include/RiskManager.h"
#include "include/Strategy.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class MarketSimulator;

class MarketMaker {
public:
    explicit MarketMaker(Instrument instrument = Instrument{});
    MarketMaker(Instrument instrument, const RiskConfig& cfg);
    MarketMaker(Instrument instrument, const RiskConfig& cfg, std::unique_ptr<Strategy> strategy);
    MarketMaker(Instrument instrument, const RiskConfig& cfg,
                std::unique_ptr<Strategy> strategy,
                std::unique_ptr<Logger> logger);
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
    Instrument instrument_;
    // At most one resting order per side. Indexed by side_index() (BUY=0,
    // SELL=1). std::optional doubles as the "is-slot-populated" flag; no
    // hash lookup, no bucket alloc.
    static constexpr std::size_t side_index(Side s) {
        return s == Side::BUY ? 0u : 1u;
    }
    std::array<std::optional<Order>, 2> active_orders;
    Ticks last_bid_price_ = 0;
    Ticks last_ask_price_ = 0;
    bool has_last_event_ = false;
    Accounting accounting_;
    RiskManager risk_manager_;
    std::unique_ptr<Strategy> strategy_;
    std::unique_ptr<Logger> logger_;
    int64_t last_processed_sequence = 0;
    uint64_t order_counter = 0;
    int total_fills = 0;

    void on_fill(const FillEvent& fill);
    void update_quotes(const MarketDataEvent& md, MarketSimulator& simulator);
    // Apply (or skip) a single side's quote against the resting slot.
    // Decision matrix: no slot → submit; same price+size → no-op;
    // anything else → amend (queue position preserved only for
    // same-price downsize per engine semantics). If amend is rejected
    // (cross-self edge), the existing resting order is left in place
    // and the rejection is logged — no cancel+resubmit fallback.
    void apply_quote(Side side, Ticks price, int qty,
                     MarketSimulator& simulator,
                     std::chrono::system_clock::time_point ts);
    void cancel_all_orders(MarketSimulator& simulator, std::chrono::system_clock::time_point now);
    uint64_t generate_order_id();
};

#endif
