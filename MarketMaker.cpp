#include "MarketMaker.h"
#include "MarketSimulator.h"
#include "include/HeuristicStrategy.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>

MarketMaker::MarketMaker()
    : strategy_(std::make_unique<HeuristicStrategy>()) {
    last_quote_time = std::chrono::system_clock::now();
}

MarketMaker::MarketMaker(const RiskConfig& cfg)
    : risk_manager_(cfg), strategy_(std::make_unique<HeuristicStrategy>()) {
    last_quote_time = std::chrono::system_clock::now();
}

MarketMaker::MarketMaker(const RiskConfig& cfg, std::unique_ptr<Strategy> strategy)
    : risk_manager_(cfg), strategy_(std::move(strategy)) {
    last_quote_time = std::chrono::system_clock::now();
}

void MarketMaker::on_market_data(const MarketDataEvent& md, MarketSimulator& simulator) {
    if (md.sequence_number != last_processed_sequence + 1 && last_processed_sequence != 0) {
        std::cout << "WARNING: Sequence gap detected. Missed "
                  << (md.sequence_number - last_processed_sequence - 1)
                  << " events\n";
    }
    last_processed_sequence = md.sequence_number;

    if (md.bid_levels.empty() || md.ask_levels.empty()) {
        std::cout << "WARNING: Empty order book detected, skipping quote update\n";
        return;
    }

    // Process fill events for our resting orders
    for (const auto& fill : md.mm_fills) {
        if (active_orders.count(fill.order_id)) {
            on_fill(fill);
        }
    }

    // Mark-to-market on each market data event
    double mid_price = (md.best_bid_price + md.best_ask_price) / 2.0;
    accounting_.mark_to_market(mid_price);

    risk_manager_.evaluate(accounting_, md, mid_price);
    if (!risk_manager_.is_quoting_allowed()) {
        cancel_all_orders(simulator, md.timestamp);
        return;
    }

    update_quotes(md, simulator);

    // Store last prices for report/mark-price (replaces unbounded market_data_log)
    last_bid_price_ = md.best_bid_price;
    last_ask_price_ = md.best_ask_price;
    has_last_event_ = true;
}

void MarketMaker::on_fill(const FillEvent& fill) {
    ++total_fills;

    // Delegate to accounting (MM resting orders are maker fills)
    accounting_.on_fill(fill.side, fill.price, fill.fill_qty, /*is_maker=*/true);

    // Update or remove active order
    auto it = active_orders.find(fill.order_id);
    if (it != active_orders.end()) {
        if (fill.leaves_qty == 0) {
            active_orders.erase(it);
        } else {
            it->second.leaves_qty = fill.leaves_qty;
            it->second.status = OrderStatus::PARTIALLY_FILLED;
        }
    }

    std::cout << "FILL: " << (fill.side == Side::BUY ? "BUY" : "SELL")
              << " " << fill.fill_qty << " @ " << std::fixed << std::setprecision(4)
              << fill.price << " (leaves=" << fill.leaves_qty
              << ") pos=" << accounting_.position()
              << " cash=" << std::setprecision(2) << accounting_.cash()
              << " realized=" << accounting_.realized_pnl()
              << " unrealized=" << accounting_.unrealized_pnl() << "\n";
}

void MarketMaker::cancel_all_orders(MarketSimulator& simulator, std::chrono::system_clock::time_point now) {
    for (auto it = active_orders.begin(); it != active_orders.end(); ) {
        risk_manager_.record_cancel(now);
        simulator.cancel_order(it->first);
        it = active_orders.erase(it);
    }
}

void MarketMaker::update_quotes(const MarketDataEvent& md, MarketSimulator& simulator) {
    // Cancel stale orders before placing new ones
    cancel_all_orders(simulator, md.timestamp);

    double best_bid = md.bid_levels[0].price;
    double best_ask = md.ask_levels[0].price;
    double mid_price = (best_bid + best_ask) / 2.0;

    // Build snapshot for strategy
    StrategySnapshot snap;
    snap.best_bid = best_bid;
    snap.best_ask = best_ask;
    snap.mid_price = mid_price;
    snap.bid_levels = md.bid_levels;
    snap.ask_levels = md.ask_levels;
    snap.trades = md.trades;
    snap.position = accounting_.position();
    snap.max_position = risk_manager_.config().max_net_position;
    snap.timestamp = md.timestamp;
    snap.sequence_number = md.sequence_number;

    QuoteDecision decision = strategy_->compute_quotes(snap);

    if (!decision.should_quote) {
        return;
    }

    // Apply risk clamps on sizes
    const auto& cfg = risk_manager_.config();
    int bid_size = std::max(cfg.min_quote_size, std::min(decision.bid_size, cfg.max_quote_size));
    int ask_size = std::max(cfg.min_quote_size, std::min(decision.ask_size, cfg.max_quote_size));

    // Submit bid
    uint64_t bid_id = generate_order_id();
    Order bid_order(bid_id, Side::BUY, decision.bid_price, bid_size, md.timestamp);
    if (simulator.submit_order(bid_order) == OrderStatus::ACKNOWLEDGED) {
        bid_order.status = OrderStatus::ACKNOWLEDGED;
        active_orders.emplace(bid_id, bid_order);
        risk_manager_.record_quote(md.timestamp);
    }

    // Submit ask
    uint64_t ask_id = generate_order_id();
    Order ask_order(ask_id, Side::SELL, decision.ask_price, ask_size, md.timestamp);
    if (simulator.submit_order(ask_order) == OrderStatus::ACKNOWLEDGED) {
        ask_order.status = OrderStatus::ACKNOWLEDGED;
        active_orders.emplace(ask_id, ask_order);
        risk_manager_.record_quote(md.timestamp);
    }

    last_quote_time = md.timestamp;
}

uint64_t MarketMaker::generate_order_id() {
    constexpr uint64_t kMmOrderTag = 1ULL << 48;
    return kMmOrderTag | static_cast<uint64_t>(++order_counter);
}

static const char* risk_state_str(RiskState s) {
    switch (s) {
        case RiskState::Normal: return "Normal";
        case RiskState::Warning: return "Warning";
        case RiskState::Breached: return "Breached";
        case RiskState::KillSwitch: return "KillSwitch";
    }
    return "Unknown";
}

void MarketMaker::report() {
    if (!has_last_event_) {
        std::cout << "No market data events logged. Report cannot be generated." << std::endl;
        return;
    }

    double mark = (last_bid_price_ + last_ask_price_) / 2.0;
    accounting_.mark_to_market(mark);

    // Inline skew formula (same as get_inventory_skew)
    const double skew_factor = 0.001;
    const double max_skew = 0.01;
    double skew = -accounting_.position() * skew_factor;
    skew = std::max(-max_skew, std::min(max_skew, skew));

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== MARKET MAKER REPORT ===" << std::endl;
    std::cout << "Position: " << accounting_.position() << " shares" << std::endl;
    std::cout << "Cash: $" << accounting_.cash() << std::endl;
    std::cout << "Mark Price: $" << mark << std::endl;
    std::cout << "Avg Entry Price: $" << accounting_.avg_entry_price() << std::endl;
    std::cout << "Realized PnL: $" << accounting_.realized_pnl() << std::endl;
    std::cout << "Unrealized PnL: $" << accounting_.unrealized_pnl() << std::endl;
    std::cout << "Total PnL: $" << accounting_.total_pnl() << std::endl;
    std::cout << "Fees: $" << accounting_.total_fees() << std::endl;
    std::cout << "Rebates: $" << accounting_.total_rebates() << std::endl;
    std::cout << "Net PnL: $" << accounting_.net_pnl() << std::endl;
    std::cout << "Gross Exposure: $" << accounting_.gross_exposure(mark) << std::endl;
    std::cout << "Net Exposure: $" << accounting_.net_exposure(mark) << std::endl;
    std::cout << "Risk State: " << risk_state_str(risk_manager_.current_state()) << std::endl;
    std::cout << "Drawdown: $" << risk_manager_.current_drawdown() << std::endl;
    std::cout << "High Water Mark: $" << risk_manager_.high_water_mark() << std::endl;
    std::cout << "Total Fills: " << total_fills << std::endl;
    std::cout << "Active Orders: " << active_orders.size() << std::endl;
    std::cout << "Strategy: " << strategy_->name() << std::endl;
    std::cout << "Inventory Skew: " << skew << std::endl;
    std::cout << "============================" << std::endl;
}

double MarketMaker::get_cash() const {
    return accounting_.cash();
}

int MarketMaker::get_inventory() const {
    return accounting_.position();
}

double MarketMaker::get_mark_price() const {
    if (!has_last_event_) return 0.0;
    return (last_bid_price_ + last_ask_price_) / 2.0;
}

double MarketMaker::get_unrealized_pnl() const {
    return accounting_.unrealized_pnl();
}

double MarketMaker::get_realized_pnl() const {
    return accounting_.realized_pnl();
}

double MarketMaker::get_total_pnl() const {
    return accounting_.net_pnl();
}

int MarketMaker::get_total_fills() const {
    return total_fills;
}

double MarketMaker::get_inventory_skew() const {
    const double skew_factor = 0.001;
    const double max_skew = 0.01;
    double skew = -accounting_.position() * skew_factor;
    return std::max(-max_skew, std::min(max_skew, skew));
}

double MarketMaker::get_fees() const {
    return accounting_.total_fees();
}

double MarketMaker::get_rebates() const {
    return accounting_.total_rebates();
}

double MarketMaker::get_avg_entry_price() const {
    return accounting_.avg_entry_price();
}

double MarketMaker::get_gross_exposure() const {
    return accounting_.gross_exposure(get_mark_price());
}

double MarketMaker::get_net_exposure() const {
    return accounting_.net_exposure(get_mark_price());
}

double MarketMaker::get_drawdown() const {
    return risk_manager_.current_drawdown();
}

double MarketMaker::get_high_water_mark() const {
    return risk_manager_.high_water_mark();
}

const char* MarketMaker::get_strategy_name() const {
    return strategy_ ? strategy_->name() : "unknown";
}

RiskState MarketMaker::get_risk_state() const {
    return risk_manager_.current_state();
}

const std::vector<RiskRuleResult>& MarketMaker::get_risk_details() const {
    return risk_manager_.last_results();
}
