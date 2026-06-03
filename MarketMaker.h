#ifndef MARKETMAKER_H
#define MARKETMAKER_H

// Polymorphic wrapper around the templated MarketMakerT<S,L> engine.
// Owns unique_ptr<Strategy> + unique_ptr<Logger> (runtime selection); the
// internal MarketMakerT<Strategy, Logger> instance points at those owned
// objects through the abstract bases, so this path pays one virtual
// dispatch per strategy / logger call. That is the WsSession escape hatch.
//
// Bench / hot-path tests bypass this wrapper and instantiate MarketMakerT
// on concrete final types, which devirtualizes the hot path. The M7
// no-virtual-dispatch CI check verifies that path.

#include "MarketDataEvent.h"
#include "Order.h"
#include "include/Accounting.h"
#include "include/Instrument.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/RiskManager.h"
#include "include/Strategy.h"

#include <memory>
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

    void on_market_data(const MarketDataEvent& md, MarketSimulator& simulator) {
        impl_.on_market_data(md, simulator);
    }
    void report() { impl_.report(); }

    double get_cash()               const { return impl_.get_cash(); }
    int    get_inventory()          const { return impl_.get_inventory(); }
    double get_mark_price()         const { return impl_.get_mark_price(); }
    double get_unrealized_pnl()     const { return impl_.get_unrealized_pnl(); }
    double get_realized_pnl()       const { return impl_.get_realized_pnl(); }
    double get_total_pnl()          const { return impl_.get_total_pnl(); }
    int    get_total_fills()        const { return impl_.get_total_fills(); }
    double get_inventory_skew()     const { return impl_.get_inventory_skew(); }
    double get_fees()               const { return impl_.get_fees(); }
    double get_rebates()            const { return impl_.get_rebates(); }
    double get_avg_entry_price()    const { return impl_.get_avg_entry_price(); }
    double get_gross_exposure()     const { return impl_.get_gross_exposure(); }
    double get_net_exposure()       const { return impl_.get_net_exposure(); }
    double get_drawdown()           const { return impl_.get_drawdown(); }
    double get_high_water_mark()    const { return impl_.get_high_water_mark(); }
    const char* get_strategy_name() const { return impl_.get_strategy_name(); }
    RiskState get_risk_state()      const { return impl_.get_risk_state(); }
    const std::vector<RiskRuleResult>& get_risk_details() const { return impl_.get_risk_details(); }

private:
    // Storage owns; impl_ borrows raw pointers. Declaration order matters:
    // strategy_ and logger_ must be constructed before impl_ takes their
    // addresses.
    std::unique_ptr<Strategy> strategy_;
    std::unique_ptr<Logger>   logger_;
    mme::MarketMakerT<Strategy, Logger> impl_;
};

#endif
