// M11/2 per-component microbench: RiskManager::evaluate in isolation.
//
// One section: evaluate(acct, md, mark_price) timed per-call into an HDR
// histogram. A representative top-of-book MarketDataEvent is built once;
// the mark price is jittered each iteration so the drawdown / exposure
// ratio branches vary. The returned RiskState is folded into the sink.
//
// Note: evaluate maintains rate-limit windows keyed off the event
// timestamp; we advance the timestamp each iteration so the rate counters
// behave as they would on a live feed rather than collapsing into one
// window. This is about exercising realistic branches, not gaming a
// number — the per-call cost is dominated by the rule evaluation.
//
// Usage: bench_risk [--iters N] [--report-path out.csv]

#include "MarketDataEvent.h"
#include "Order.h"
#include "bench/bench_common.h"
#include "include/Accounting.h"
#include "include/HdrPerformanceModule.h"
#include "include/Instrument.h"
#include "include/RiskManager.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>

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
            std::cout << "Usage: bench_risk [--iters N] [--report-path out.csv]\n";
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

    Instrument instrument(0.01);
    Accounting acct(instrument, 1'000'000.0);
    // Put the book in a non-trivial position so exposure rules do work.
    acct.on_fill(Side::BUY, 100000, 200, true);
    acct.mark_to_market(1000.0);

    RiskConfig cfg;
    RiskManager risk(instrument, cfg);

    MarketDataEvent md;
    md.instrument = "BENCH";
    md.best_bid_price = 99995;
    md.best_ask_price = 100005;
    md.best_bid_size = 300;
    md.best_ask_size = 300;
    auto base_ts = std::chrono::system_clock::now();
    md.timestamp = base_ts;
    md.sequence_number = 0;

    mme::HdrPerformanceModule perf;
    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        md.timestamp = base_ts + std::chrono::microseconds(i);
        md.sequence_number = i;
        double mark = 1000.0 + static_cast<double>(i % 50) * 0.01;

        auto t0 = now();
        RiskState s = risk.evaluate(acct, md, mark);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
        g_sink ^= static_cast<std::int64_t>(s);
    }
    perf.set_wall_time(now() - wall0);
    emit_section(*csv, "risk_evaluate", perf);

    if (g_sink == 0x7fffffffffffffffLL) std::cerr << "sink\n";
    return 0;
}
