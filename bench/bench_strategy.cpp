// M11/2 per-component microbench: HeuristicStrategy::compute_quotes in
// isolation, called on the concrete type (not through the Strategy vtable)
// so the measured cost matches the devirtualized hot path the templated
// MarketMakerT<S,L> compiles down to.
//
// One section: compute_quotes timed per-call into an HDR histogram. The
// snapshot's mid / position / book depth are jittered each iteration so
// the inventory-skew branches are exercised. Emitted quote prices are
// folded into the sink to defeat DCE.
//
// Usage: bench_strategy [--iters N] [--report-path out.csv]

#include "MarketDataEvent.h"
#include "Order.h"
#include "bench/bench_common.h"
#include "include/HdrPerformanceModule.h"
#include "include/HeuristicStrategy.h"
#include "include/Instrument.h"
#include "include/Strategy.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace {

using mme::bench::emit_section;
using mme::bench::g_sink;
using mme::bench::now;
using mme::bench::ns_between;

}  // namespace

int main(int argc, char* argv[]) {
    int iters = 1'000'000;
    std::string report_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--iters" && i + 1 < argc) {
            iters = std::stoi(argv[++i]);
        } else if (a == "--report-path" && i + 1 < argc) {
            report_path = argv[++i];
        } else if (a == "--help") {
            std::cout << "Usage: bench_strategy [--iters N] [--report-path out.csv]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return 1;
        }
    }

    std::ofstream file;
    std::ostream* csv = &std::cout;
    if (!report_path.empty()) {
        file.open(report_path);
        if (!file) {
            std::cerr << "Cannot open report path: " << report_path << "\n";
            return 1;
        }
        csv = &file;
    }

    const auto ts = std::chrono::system_clock::now();
    // A few levels per side so the spans are non-empty and realistic.
    std::vector<OrderLevel> bids;
    std::vector<OrderLevel> asks;
    for (int d = 0; d < 5; ++d) {
        bids.emplace_back(99995 - d, 200 + d * 10, 1000 + d, ts);
        asks.emplace_back(100005 + d, 200 + d * 10, 2000 + d, ts);
    }
    std::vector<Trade> trades;

    HeuristicStrategy strategy;

    StrategySnapshot snap;
    snap.best_bid = 99995;
    snap.best_ask = 100005;
    snap.bid_levels = std::span<const OrderLevel>(bids);
    snap.ask_levels = std::span<const OrderLevel>(asks);
    snap.trades = std::span<const Trade>(trades);
    snap.max_position = 1000;
    snap.tick_size = 0.01;
    snap.timestamp = ts;

    mme::HdrPerformanceModule perf;
    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        // Jitter mid + inventory so the skew / clamp branches vary.
        snap.mid_price = 1000.0 + static_cast<double>(i % 50) * 0.01;
        snap.position = (i % 2001) - 1000;
        snap.sequence_number = i;

        auto t0 = now();
        QuoteDecision q = strategy.compute_quotes(snap);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
        g_sink ^= static_cast<std::int64_t>(q.bid_price) ^
                  static_cast<std::int64_t>(q.ask_price) ^ (q.should_quote ? 1 : 0);
    }
    perf.set_wall_time(now() - wall0);
    emit_section(*csv, "strategy_compute_quotes", perf);

    if (g_sink == 0x7fffffffffffffffLL) std::cerr << "sink\n";
    return 0;
}
