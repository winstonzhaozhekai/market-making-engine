#ifndef LOGGER_H
#define LOGGER_H

#include "../Order.h"
#include "Instrument.h"

#include <cstdint>
#include <iomanip>
#include <iostream>

// Hot-path logger. Methods are sized for scalar args so the M6 SPSC ring
// buffer can serialize them without allocating or formatting on the
// producer thread. M5 ships the NullLogger (zero-overhead) and a
// StdoutLogger that reproduces the prior std::cout output for the human
// CLI. Virtual dispatch is fine for M5 — the cost we're chasing here is
// allocation, not dispatch. M7 will template MarketMaker on Strategy and
// can template on Logger in the same pass.
class Logger {
public:
    virtual ~Logger() = default;
    virtual void on_fill(const FillEvent& fill, const Instrument& ins,
                         int position, double cash,
                         double realized_pnl, double unrealized_pnl) = 0;
    virtual void on_sequence_gap(int64_t missed) = 0;
    virtual void on_empty_book() = 0;
    virtual void on_amend_rejected(uint64_t order_id) = 0;
};

class NullLogger final : public Logger {
public:
    void on_fill(const FillEvent&, const Instrument&,
                 int, double, double, double) override {}
    void on_sequence_gap(int64_t) override {}
    void on_empty_book() override {}
    void on_amend_rejected(uint64_t) override {}
};

class StdoutLogger final : public Logger {
public:
    void on_fill(const FillEvent& fill, const Instrument& ins,
                 int position, double cash,
                 double realized_pnl, double unrealized_pnl) override {
        std::cout << "FILL: " << (fill.side == Side::BUY ? "BUY" : "SELL")
                  << " " << fill.fill_qty << " @ "
                  << std::fixed << std::setprecision(4)
                  << ins.to_price(fill.price)
                  << " (leaves=" << fill.leaves_qty
                  << ") pos=" << position
                  << " cash=" << std::setprecision(2) << cash
                  << " realized=" << realized_pnl
                  << " unrealized=" << unrealized_pnl << "\n";
    }
    void on_sequence_gap(int64_t missed) override {
        std::cout << "WARNING: Sequence gap detected. Missed "
                  << missed << " events\n";
    }
    void on_empty_book() override {
        std::cout << "WARNING: Empty order book detected, skipping quote update\n";
    }
    void on_amend_rejected(uint64_t order_id) override {
        std::cout << "WARNING: Amend rejected (cross-self) for order "
                  << order_id << "\n";
    }
};

#endif // LOGGER_H
