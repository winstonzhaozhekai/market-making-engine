// M11/2 per-component microbench: Accounting::on_fill in isolation.
//
// One section: on_fill timed per-call into an HDR histogram. Fills
// alternate BUY/SELL and maker/taker so the open/close, cost-basis and
// rebate/fee branches all get exercised rather than one hot branch. The
// resulting realized PnL is folded into the sink to defeat DCE.
//
// Usage: bench_accounting [--iters N] [--report-path out.csv]

#include "bench/bench_common.h"
#include "include/Accounting.h"
#include "include/HdrPerformanceModule.h"
#include "include/Instrument.h"
#include "Order.h"

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
            std::cout << "Usage: bench_accounting [--iters N] [--report-path out.csv]\n";
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
    FeeSchedule fees;
    fees.maker_rebate_per_share = 0.0020;
    fees.taker_fee_per_share = 0.0030;
    Accounting acct(instrument, 1'000'000.0, fees);

    mme::HdrPerformanceModule perf;
    const Ticks base = 100000;

    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        Side side = (i & 1) ? Side::SELL : Side::BUY;
        bool is_maker = (i & 2) == 0;
        Ticks px = base + static_cast<Ticks>(i % 20) - 10;
        int qty = 100;

        auto t0 = now();
        acct.on_fill(side, px, qty, is_maker);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
    }
    perf.set_wall_time(now() - wall0);

    g_sink ^= static_cast<std::int64_t>(acct.realized_pnl());
    emit_section(*csv, "accounting_on_fill", perf);

    if (g_sink == 0x7fffffffffffffffLL) std::cerr << "sink\n";
    return 0;
}
