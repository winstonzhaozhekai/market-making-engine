#ifndef ACCOUNTING_H
#define ACCOUNTING_H

#include "Order.h"
#include <algorithm>
#include <cmath>

struct FeeSchedule {
    double maker_rebate_per_share = 0.0;
    double taker_fee_per_share = 0.0;
    double fee_bps = 0.0;  // basis-point fee on notional
};

class Accounting {
public:
    explicit Accounting(double initial_capital, FeeSchedule fees = {})
        : initial_capital_(initial_capital), cash_(initial_capital), fees_(fees) {}

    void on_fill(Side side, double price, int qty, bool is_maker) {
        double notional = price * qty;

        // Apply fees/rebates
        double fee = notional * (fees_.fee_bps / 10000.0);
        if (is_maker) {
            double rebate = fees_.maker_rebate_per_share * qty;
            total_rebates_ += rebate;
            fee -= rebate;  // net fee after rebate
        } else {
            fee += fees_.taker_fee_per_share * qty;
        }
        total_fees_ += fee;

        if (side == Side::BUY) {
            // Buying: cash goes down, position goes up
            cash_ -= notional;
            if (position_ >= 0) {
                // Adding to long or opening long: increase cost basis
                cost_basis_ += notional;
            } else {
                // Closing short (or flipping to long)
                int close_qty = std::min(qty, -position_);
                int open_qty = qty - close_qty;
                double avg_entry = avg_entry_price();
                // Realized PnL on closing short: sold high, buying back lower
                realized_pnl_ += (avg_entry - price) * close_qty;
                if (open_qty > 0) {
                    // Flipped to long: reset cost basis for new long position
                    cost_basis_ = price * open_qty;
                } else {
                    // Partially or fully closed short
                    cost_basis_ -= avg_entry * close_qty;
                }
            }
            position_ += qty;
        } else {
            // Selling: cash goes up, position goes down
            cash_ += notional;
            if (position_ <= 0) {
                // Adding to short or opening short: increase cost basis (absolute)
                cost_basis_ += notional;
            } else {
                // Closing long (or flipping to short)
                int close_qty = std::min(qty, position_);
                int open_qty = qty - close_qty;
                double avg_entry = avg_entry_price();
                // Realized PnL on closing long: bought low, selling higher
                realized_pnl_ += (price - avg_entry) * close_qty;
                if (open_qty > 0) {
                    // Flipped to short: reset cost basis for new short position
                    cost_basis_ = price * open_qty;
                } else {
                    // Partially or fully closed long
                    cost_basis_ -= avg_entry * close_qty;
                }
            }
            position_ -= qty;
        }

        // If position is zero, ensure cost basis is clean
        if (position_ == 0) {
            cost_basis_ = 0.0;
        }

        // Update mark to fill price for unrealized calc
        mark_to_market(price);
    }

    void mark_to_market(double mark_price) {
        mark_price_ = mark_price;
        if (position_ != 0) {
            double avg = avg_entry_price();
            if (position_ > 0) {
                unrealized_pnl_ = (mark_price - avg) * position_;
            } else {
                unrealized_pnl_ = (avg - mark_price) * (-position_);
            }
        } else {
            unrealized_pnl_ = 0.0;
        }
    }

    // Queries
    double realized_pnl() const { return realized_pnl_; }
    double unrealized_pnl() const { return unrealized_pnl_; }
    double total_pnl() const { return realized_pnl_ + unrealized_pnl_; }
    double net_pnl() const { return total_pnl() - total_fees_ + total_rebates_; }
    double total_fees() const { return total_fees_; }
    double total_rebates() const { return total_rebates_; }

    double avg_entry_price() const {
        if (position_ == 0) return 0.0;
        return cost_basis_ / std::abs(position_);
    }

    double cost_basis() const { return cost_basis_; }
    int position() const { return position_; }
    double cash() const { return cash_; }
    double initial_capital() const { return initial_capital_; }

    double gross_exposure(double mark_price) const {
        return std::abs(position_) * mark_price;
    }

    double net_exposure(double mark_price) const {
        return position_ * mark_price;
    }

    void reset_daily() {
        realized_pnl_ = 0.0;
        unrealized_pnl_ = 0.0;
        total_fees_ = 0.0;
        total_rebates_ = 0.0;
    }

private:
    double initial_capital_;
    double cash_;
    int    position_ = 0;
    double cost_basis_ = 0.0;
    double realized_pnl_ = 0.0;
    double unrealized_pnl_ = 0.0;
    double total_fees_ = 0.0;
    double total_rebates_ = 0.0;
    double mark_price_ = 0.0;
    FeeSchedule fees_;
};

#endif // ACCOUNTING_H
