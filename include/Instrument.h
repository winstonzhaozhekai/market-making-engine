#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <cmath>
#include <cstdint>
#include <stdexcept>

// Integer ticks are the canonical price representation on the hot path.
// `double` only crosses module boundaries (CLI, JSON, replay log, UI).
using Ticks = std::int64_t;

// Single-instrument design (D11). One Instrument value is constructed from
// SimulationConfig and shared by reference to MatchingEngine, MarketSimulator,
// MarketMaker, Accounting, RiskManager.
struct Instrument {
    double tick_size;

    constexpr explicit Instrument(double tick_size_ = 0.01) : tick_size(tick_size_) {
        if (!(tick_size_ > 0.0)) {
            throw std::invalid_argument("Instrument tick_size must be positive");
        }
    }

    // Snap a dollar-denominated price to the tick grid. Round-half-to-even
    // would be more impartial under noise but std::lround (round-half-away-
    // from-zero) is sufficient for this simulator: input prices are always
    // positive and the snap is deterministic for a given input.
    Ticks to_ticks(double price) const {
        return static_cast<Ticks>(std::lround(price / tick_size));
    }

    constexpr double to_price(Ticks t) const {
        return static_cast<double>(t) * tick_size;
    }
};

#endif // INSTRUMENT_H
