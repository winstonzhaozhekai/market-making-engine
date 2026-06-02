#ifndef STRATEGY_H
#define STRATEGY_H

#include "../MarketDataEvent.h"
#include "Instrument.h"
#include <chrono>
#include <span>

// StrategySnapshot is the read-only view of market state passed to a strategy.
// best_bid / best_ask are integer ticks (the exchange's price grid). mid_price
// is double-precision dollars — it is a derived reference quantity that does
// not need to land on the tick grid (no order is ever priced at the mid).
//
// bid_levels / ask_levels / trades are non-owning views over the underlying
// MarketDataEvent that drove this tick. The snapshot is constructed and
// consumed entirely within MarketMaker::on_market_data, so the spans are
// valid for the strategy's compute_quotes call.
struct StrategySnapshot {
    Ticks best_bid = 0;
    Ticks best_ask = 0;
    double mid_price = 0.0;
    std::span<const OrderLevel> bid_levels;
    std::span<const OrderLevel> ask_levels;
    std::span<const Trade>      trades;
    int position = 0;
    int max_position = 1000;
    double tick_size = 0.01;
    std::chrono::system_clock::time_point timestamp;
    int64_t sequence_number = 0;
};

// QuoteDecision: bid/ask are emitted as Ticks (snapped by the strategy).
struct QuoteDecision {
    Ticks bid_price = 0;
    Ticks ask_price = 0;
    int bid_size = 0;
    int ask_size = 0;
    bool should_quote = true;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual QuoteDecision compute_quotes(const StrategySnapshot& snapshot) = 0;
    virtual const char* name() const = 0;
};

#endif // STRATEGY_H
