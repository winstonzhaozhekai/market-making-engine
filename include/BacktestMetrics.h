#ifndef MME_BACKTEST_METRICS_H
#define MME_BACKTEST_METRICS_H

// Backtest metrics primitives consumed by `bench/itch_replay`. Holds the
// raw event-timestamped traces a caller produces (mid prices, PnL
// samples, MM fills) and derives the standard MM-evaluation summary
// numbers: max drawdown, annualized Sharpe, post-fill adverse-selection
// drift, and fill rate.
//
// The module is deliberately a thin accumulator-+-computer. It does NOT
// own the Accounting object, does not subscribe to a market simulator,
// and does not emit any output — `bench/itch_replay.cpp` glues those
// pieces together and writes the CSV report.
//
// Time convention: all timestamps are int64 nanoseconds since some
// fixed epoch (caller's choice; for ITCH replay we use the tape's
// nanos-since-midnight directly). Adverse-selection windows and the
// Sharpe sample interval are likewise expressed in nanoseconds so the
// module is unit-agnostic.

#include "../Order.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace mme {

struct MidSample {
    std::int64_t ts_ns;
    double       mid;
};

struct PnlSample {
    std::int64_t ts_ns;
    double       pnl;
};

struct MmFillRecord {
    std::int64_t ts_ns;
    Side         side;
    double       price;
    int          qty;
};

class BacktestMetrics {
public:
    // `sharpe_sample_interval_ns` is the Δt assumed between consecutive
    // pnl samples for the Sharpe computation. Default 1 s.
    // `annual_seconds` is the number of trading seconds per year — the
    // default `252 * 6.5 * 3600 = 5,896,800` is the canonical US-equity
    // HFT convention. Override for other markets / sampling intervals.
    explicit BacktestMetrics(
        std::int64_t sharpe_sample_interval_ns = 1'000'000'000,
        double       annual_seconds            = 252.0 * 6.5 * 3600.0)
        : sharpe_interval_ns_(sharpe_sample_interval_ns),
          annual_seconds_(annual_seconds) {}

    void record_mid(std::int64_t ts_ns, double mid) {
        mid_trace_.push_back({ts_ns, mid});
    }

    void record_pnl(std::int64_t ts_ns, double pnl) {
        pnl_trace_.push_back({ts_ns, pnl});
    }

    void record_fill(std::int64_t ts_ns, Side side, double price, int qty) {
        fills_.push_back({ts_ns, side, price, qty});
    }

    void record_inventory(std::int64_t ts_ns, int inventory) {
        inventory_trace_.push_back({ts_ns, static_cast<double>(inventory)});
    }

    void record_quote_posted() { ++quotes_posted_; }

    std::size_t fills_count()    const { return fills_.size(); }
    std::size_t quotes_posted()  const { return quotes_posted_; }
    double      fill_rate()      const {
        if (quotes_posted_ == 0) return 0.0;
        return static_cast<double>(fills_.size())
             / static_cast<double>(quotes_posted_);
    }

    const std::vector<MidSample>&    mid_trace()       const { return mid_trace_; }
    const std::vector<PnlSample>&    pnl_trace()       const { return pnl_trace_; }
    const std::vector<MmFillRecord>& fills()           const { return fills_; }
    const std::vector<PnlSample>&    inventory_trace() const { return inventory_trace_; }

    // Max drawdown over the PnL trace as recorded. Returns 0 if the
    // trace is empty or monotonically non-decreasing.
    double max_drawdown() const {
        double running_max = -std::numeric_limits<double>::infinity();
        double max_dd      = 0.0;
        for (const auto& s : pnl_trace_) {
            running_max = std::max(running_max, s.pnl);
            max_dd      = std::max(max_dd, running_max - s.pnl);
        }
        return max_dd;
    }

    // Annualized Sharpe over the PnL trace. Treats consecutive pnl
    // samples as one `sharpe_interval_ns` apart and computes the
    // per-period simple return series (pnl_i - pnl_{i-1}) divided by
    // pnl_{i-1} where it is non-zero, falling back to absolute deltas
    // when the prior PnL is zero. Returns 0 if fewer than 2 samples or
    // a degenerate variance.
    double sharpe_annualized() const {
        if (pnl_trace_.size() < 2) return 0.0;
        std::vector<double> returns;
        returns.reserve(pnl_trace_.size() - 1);
        for (std::size_t i = 1; i < pnl_trace_.size(); ++i) {
            const double prev = pnl_trace_[i - 1].pnl;
            const double cur  = pnl_trace_[i].pnl;
            const double r    = (std::abs(prev) > 1e-9)
                                    ? (cur - prev) / std::abs(prev)
                                    : (cur - prev);
            returns.push_back(r);
        }
        const double mean = std::accumulate(returns.begin(), returns.end(), 0.0)
                          / static_cast<double>(returns.size());
        double sumsq = 0.0;
        for (double r : returns) {
            const double d = r - mean;
            sumsq += d * d;
        }
        const double var = sumsq / static_cast<double>(returns.size());
        if (var <= 0.0) return 0.0;
        const double stddev = std::sqrt(var);
        const double samples_per_year =
            annual_seconds_ * 1e9 / static_cast<double>(sharpe_interval_ns_);
        return (mean / stddev) * std::sqrt(samples_per_year);
    }

    // Per-fill adverse-selection drift at delta_ns after each fill:
    //   drift_i = sign(side_i) * (mid(t_i + Δ) - fill_price_i)
    // where sign(BUY) = +1 and sign(SELL) = -1, so positive drift means
    // MM was on the right side of the move. Fills whose lookahead falls
    // off the end of the mid trace are excluded.
    std::vector<double> adverse_selection_dollars(std::int64_t delta_ns) const {
        std::vector<double> drifts;
        drifts.reserve(fills_.size());
        for (const auto& f : fills_) {
            const std::int64_t target = f.ts_ns + delta_ns;
            auto it = std::lower_bound(
                mid_trace_.begin(), mid_trace_.end(), target,
                [](const MidSample& s, std::int64_t v) { return s.ts_ns < v; });
            if (it == mid_trace_.end()) continue;
            const double sign  = (f.side == Side::BUY) ? 1.0 : -1.0;
            const double drift = sign * (it->mid - f.price);
            drifts.push_back(drift);
        }
        return drifts;
    }

    // Average of `adverse_selection_dollars(delta_ns)`, or 0 if no
    // qualifying fills exist.
    double avg_adverse_selection(std::int64_t delta_ns) const {
        const auto drifts = adverse_selection_dollars(delta_ns);
        if (drifts.empty()) return 0.0;
        return std::accumulate(drifts.begin(), drifts.end(), 0.0)
             / static_cast<double>(drifts.size());
    }

private:
    std::int64_t sharpe_interval_ns_;
    double       annual_seconds_;

    std::vector<MidSample>    mid_trace_;
    std::vector<PnlSample>    pnl_trace_;
    std::vector<MmFillRecord> fills_;
    std::vector<PnlSample>    inventory_trace_;
    std::size_t               quotes_posted_ = 0;
};

}  // namespace mme

#endif  // MME_BACKTEST_METRICS_H
