#ifndef HEURISTIC_STRATEGY_H
#define HEURISTIC_STRATEGY_H

#include "Strategy.h"
#include <algorithm>
#include <cmath>

class HeuristicStrategy : public Strategy {
public:
    QuoteDecision compute_quotes(const StrategySnapshot& snap) override {
        const double base_spread = 0.02;
        const double skew_factor = 0.001;
        const double max_skew = 0.01;
        const int base_size = 5;
        const double size_factor = 0.1;

        double inv_skew = -snap.position * skew_factor;
        inv_skew = std::max(-max_skew, std::min(max_skew, inv_skew));

        double bid_price = snap.mid_price - base_spread / 2.0 + inv_skew;
        double ask_price = snap.mid_price + base_spread / 2.0 + inv_skew;

        // Bid size: based on top-of-book depth on bid side
        int bid_depth = 0;
        if (!snap.bid_levels.empty()) {
            bid_depth = snap.bid_levels[0].size;
        }
        double bid_inv_factor = 1.0 - std::abs(snap.position) / static_cast<double>(snap.max_position);
        bid_inv_factor = std::max(0.1, bid_inv_factor);
        int bid_size = static_cast<int>(base_size * (1.0 + bid_depth * size_factor) * bid_inv_factor);
        bid_size = std::max(1, bid_size);

        // Ask size: based on top-of-book depth on ask side
        int ask_depth = 0;
        if (!snap.ask_levels.empty()) {
            ask_depth = snap.ask_levels[0].size;
        }
        double ask_inv_factor = 1.0 - std::abs(snap.position) / static_cast<double>(snap.max_position);
        ask_inv_factor = std::max(0.1, ask_inv_factor);
        int ask_size = static_cast<int>(base_size * (1.0 + ask_depth * size_factor) * ask_inv_factor);
        ask_size = std::max(1, ask_size);

        QuoteDecision decision;
        decision.bid_price = bid_price;
        decision.ask_price = ask_price;
        decision.bid_size = bid_size;
        decision.ask_size = ask_size;
        decision.should_quote = true;
        return decision;
    }

    const char* name() const override { return "heuristic"; }
};

#endif // HEURISTIC_STRATEGY_H
