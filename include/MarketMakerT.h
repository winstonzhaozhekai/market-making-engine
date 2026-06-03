#ifndef MARKET_MAKER_T_H
#define MARKET_MAKER_T_H

// Templated market-maker engine. StrategyT and LoggerT are non-owning
// pointers passed in by the caller; instantiating on concrete final types
// (e.g. MarketMakerT<HeuristicStrategy, NullLogger>) lets the compiler
// devirtualize compute_quotes / on_fill / on_sequence_gap / on_empty_book /
// on_amend_rejected — no callq * on the on_market_data hot path.
//
// The non-template MarketMaker wrapper instantiates this as
// MarketMakerT<Strategy, Logger> over the abstract bases, preserving the
// virtual escape hatch used by WsSession runtime strategy selection.
//
// All hot-path logic (apply_quote, update_quotes, on_fill, etc.) lives
// here; MarketMaker.cpp now contains only forwarding wrappers.

#include "../MarketDataEvent.h"
#include "../MarketSimulator.h"
#include "../Order.h"
#include "Accounting.h"
#include "Instrument.h"
#include "Logger.h"
#include "RiskManager.h"
#include "Strategy.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <vector>

namespace mme {

inline constexpr double kInitialCapital = 100000.0;

inline const char* risk_state_str(RiskState s) {
    switch (s) {
        case RiskState::Normal:     return "Normal";
        case RiskState::Warning:    return "Warning";
        case RiskState::Breached:   return "Breached";
        case RiskState::KillSwitch: return "KillSwitch";
    }
    return "Unknown";
}

template <class StrategyT, class LoggerT>
class MarketMakerT {
public:
    // Non-owning. Caller must keep *strategy and *logger alive for the
    // lifetime of this MarketMakerT.
    MarketMakerT(Instrument instrument, const RiskConfig& cfg,
                 StrategyT* strategy, LoggerT* logger)
        : instrument_(instrument),
          accounting_(instrument, kInitialCapital),
          risk_manager_(instrument, cfg),
          strategy_(strategy),
          logger_(logger) {}

    void on_market_data(const MarketDataEvent& md, MarketSimulator& simulator) {
        if (md.sequence_number != last_processed_sequence_ + 1 && last_processed_sequence_ != 0) {
            logger_->on_sequence_gap(md.sequence_number - last_processed_sequence_ - 1);
        }
        last_processed_sequence_ = md.sequence_number;

        if (md.bid_levels.empty() || md.ask_levels.empty()) {
            logger_->on_empty_book();
            return;
        }

        // Process every reported fill: accounting must book it even when
        // MM has already cancelled the slot (M8 ack/matching latency can
        // make a fill arrive after MM's cancel landed). Slot reconciliation
        // is guarded inside on_fill().
        for (const auto& fill : md.mm_fills) {
            on_fill(fill);
        }

        double mid_dollars = instrument_.to_price(md.best_bid_price + md.best_ask_price) / 2.0;
        accounting_.mark_to_market(mid_dollars);

        risk_manager_.evaluate(accounting_, md, mid_dollars);
        if (!risk_manager_.is_quoting_allowed()) {
            cancel_all_orders(simulator, md.timestamp);
            return;
        }

        update_quotes(md, simulator);

        last_bid_price_ = md.best_bid_price;
        last_ask_price_ = md.best_ask_price;
        has_last_event_ = true;
    }

    void report() {
        if (!has_last_event_) {
            std::cout << "No market data events logged. Report cannot be generated." << std::endl;
            return;
        }

        double mark = instrument_.to_price(last_bid_price_ + last_ask_price_) / 2.0;
        accounting_.mark_to_market(mark);

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
        std::cout << "Total Fills: " << total_fills_ << std::endl;
        int active_count = (active_orders_[0] ? 1 : 0) + (active_orders_[1] ? 1 : 0);
        std::cout << "Active Orders: " << active_count << std::endl;
        std::cout << "Strategy: " << strategy_->name() << std::endl;
        std::cout << "Inventory Skew: " << skew << std::endl;
        std::cout << "============================" << std::endl;
    }

    double get_cash() const { return accounting_.cash(); }
    int    get_inventory() const { return accounting_.position(); }
    double get_mark_price() const {
        if (!has_last_event_) return 0.0;
        return instrument_.to_price(last_bid_price_ + last_ask_price_) / 2.0;
    }
    double get_unrealized_pnl() const { return accounting_.unrealized_pnl(); }
    double get_realized_pnl()   const { return accounting_.realized_pnl(); }
    double get_total_pnl()      const { return accounting_.net_pnl(); }
    int    get_total_fills()    const { return total_fills_; }
    double get_inventory_skew() const {
        const double skew_factor = 0.001;
        const double max_skew = 0.01;
        double skew = -accounting_.position() * skew_factor;
        return std::max(-max_skew, std::min(max_skew, skew));
    }
    double get_fees()             const { return accounting_.total_fees(); }
    double get_rebates()          const { return accounting_.total_rebates(); }
    double get_avg_entry_price()  const { return accounting_.avg_entry_price(); }
    double get_gross_exposure()   const { return accounting_.gross_exposure(get_mark_price()); }
    double get_net_exposure()     const { return accounting_.net_exposure(get_mark_price()); }
    double get_drawdown()         const { return risk_manager_.current_drawdown(); }
    double get_high_water_mark()  const { return risk_manager_.high_water_mark(); }
    const char* get_strategy_name() const { return strategy_ ? strategy_->name() : "unknown"; }
    RiskState get_risk_state()    const { return risk_manager_.current_state(); }
    const std::vector<RiskRuleResult>& get_risk_details() const {
        return risk_manager_.last_results();
    }

private:
    static constexpr std::size_t side_index(Side s) {
        return s == Side::BUY ? 0u : 1u;
    }

    void on_fill(const FillEvent& fill) {
        ++total_fills_;

        accounting_.on_fill(fill.side, fill.price, fill.fill_qty, /*is_maker=*/true);

        auto& slot = active_orders_[side_index(fill.side)];
        if (slot && slot->order_id == fill.order_id) {
            if (fill.leaves_qty == 0) {
                slot.reset();
            } else {
                slot->leaves_qty = fill.leaves_qty;
                slot->status = OrderStatus::PARTIALLY_FILLED;
            }
        }

        logger_->on_fill(fill, instrument_, accounting_.position(), accounting_.cash(),
                         accounting_.realized_pnl(), accounting_.unrealized_pnl());
    }

    void cancel_all_orders(MarketSimulator& simulator, std::chrono::system_clock::time_point now) {
        for (auto& slot : active_orders_) {
            if (slot) {
                risk_manager_.record_cancel(now);
                simulator.cancel_order(slot->order_id);
                slot.reset();
            }
        }
    }

    void update_quotes(const MarketDataEvent& md, MarketSimulator& simulator) {
        Ticks best_bid = md.bid_levels[0].price;
        Ticks best_ask = md.ask_levels[0].price;
        double mid_dollars = instrument_.to_price(best_bid + best_ask) / 2.0;

        StrategySnapshot snap;
        snap.best_bid = best_bid;
        snap.best_ask = best_ask;
        snap.mid_price = mid_dollars;
        snap.bid_levels = md.bid_levels;
        snap.ask_levels = md.ask_levels;
        snap.trades = md.trades;
        snap.position = accounting_.position();
        snap.max_position = risk_manager_.config().max_net_position;
        snap.tick_size = instrument_.tick_size;
        snap.timestamp = md.timestamp;
        snap.sequence_number = md.sequence_number;

        QuoteDecision decision = strategy_->compute_quotes(snap);

        if (!decision.should_quote) {
            cancel_all_orders(simulator, md.timestamp);
            return;
        }

        const auto& cfg = risk_manager_.config();
        int bid_size = std::max(cfg.min_quote_size, std::min(decision.bid_size, cfg.max_quote_size));
        int ask_size = std::max(cfg.min_quote_size, std::min(decision.ask_size, cfg.max_quote_size));

        apply_quote(Side::BUY,  decision.bid_price, bid_size, simulator, md.timestamp);
        apply_quote(Side::SELL, decision.ask_price, ask_size, simulator, md.timestamp);
    }

    void apply_quote(Side side, Ticks price, int qty,
                     MarketSimulator& simulator,
                     std::chrono::system_clock::time_point ts) {
        auto& slot = active_orders_[side_index(side)];

        if (!slot) {
            uint64_t id = generate_order_id();
            Order order(id, side, price, qty, ts);
            if (simulator.submit_order(order, OrderType::POST_ONLY) == OrderStatus::ACKNOWLEDGED) {
                order.status = OrderStatus::ACKNOWLEDGED;
                slot = order;
                risk_manager_.record_quote(ts);
            }
            return;
        }

        if (slot->price == price && slot->leaves_qty == qty) {
            return;
        }

        if (simulator.amend_order(slot->order_id, price, qty, ts)) {
            slot->price      = price;
            slot->leaves_qty = qty;
            slot->updated_at = ts;
            risk_manager_.record_quote(ts);
        } else {
            logger_->on_amend_rejected(slot->order_id);
        }
    }

    uint64_t generate_order_id() {
        constexpr uint64_t kMmOrderTag = 1ULL << 48;
        return kMmOrderTag | static_cast<uint64_t>(++order_counter_);
    }

    Instrument instrument_;
    std::array<std::optional<Order>, 2> active_orders_{};
    Ticks last_bid_price_ = 0;
    Ticks last_ask_price_ = 0;
    bool has_last_event_ = false;
    Accounting accounting_;
    RiskManager risk_manager_;
    StrategyT* strategy_;
    LoggerT*   logger_;
    int64_t last_processed_sequence_ = 0;
    uint64_t order_counter_ = 0;
    int total_fills_ = 0;
};

} // namespace mme

#endif // MARKET_MAKER_T_H
