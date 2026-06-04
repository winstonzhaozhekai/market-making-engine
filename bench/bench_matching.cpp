// M11/2 per-component microbench: MatchingEngine hot path in isolation.
//
// Three sections, each timed per-call into an HDR histogram:
//   add_order    — POST_ONLY rest into a one-sided (never-crossing) book
//   cancel_order — O(1) cancel by id from a pre-filled book
//   amend_order  — same-price qty-down amend (queue position preserved)
//
// Each section uses a fresh engine so the measured op is not contaminated
// by teardown of the previous one. Results are XOR-folded into the shared
// volatile sink so the optimizer cannot elide the calls.
//
// Usage: bench_matching [--iters N] [--report-path out.csv]

#include "MatchingEngine.h"
#include "Order.h"
#include "bench/bench_common.h"
#include "include/HdrPerformanceModule.h"
#include "include/Instrument.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

namespace {

using mme::bench::emit_section;
using mme::bench::g_sink;
using mme::bench::now;
using mme::bench::ns_between;

constexpr Ticks kMid = 100000;  // arbitrary mid in ticks ($1000.00 @ 1c)

Order make_bid(std::uint64_t id, Ticks price, int qty,
               std::chrono::system_clock::time_point ts) {
    return Order(id, Side::BUY, price, qty, ts);
}

// Spread the rested orders across a 1000-tick band below mid so the book
// has many populated price levels, not one giant FIFO — closer to a real
// book's shape and exercises the std::map level index.
Ticks bid_price_for(std::uint64_t i) {
    return kMid - 1 - static_cast<Ticks>(i % 1000);
}

void bench_add(int iters, std::ostream& csv) {
    MatchingEngine engine;
    mme::HdrPerformanceModule perf;
    const auto ts = std::chrono::system_clock::now();

    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        Order o = make_bid(static_cast<std::uint64_t>(i) + 1,
                           bid_price_for(static_cast<std::uint64_t>(i)), 100, ts);
        auto t0 = now();
        SubmitResult r = engine.add_order(o, OrderType::POST_ONLY);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
        g_sink ^= static_cast<std::int64_t>(r.status);
    }
    perf.set_wall_time(now() - wall0);
    emit_section(csv, "matching_add_order", perf);
}

void bench_cancel(int iters, std::ostream& csv) {
    MatchingEngine engine;
    const auto ts = std::chrono::system_clock::now();
    for (int i = 0; i < iters; ++i) {
        engine.add_order(make_bid(static_cast<std::uint64_t>(i) + 1,
                                  bid_price_for(static_cast<std::uint64_t>(i)), 100, ts),
                         OrderType::POST_ONLY);
    }

    mme::HdrPerformanceModule perf;
    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        auto id = static_cast<std::uint64_t>(i) + 1;
        auto t0 = now();
        bool ok = engine.cancel_order(id);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
        g_sink ^= ok ? 1 : 0;
    }
    perf.set_wall_time(now() - wall0);
    emit_section(csv, "matching_cancel_order", perf);
}

void bench_amend(int iters, std::ostream& csv) {
    MatchingEngine engine;
    const auto ts = std::chrono::system_clock::now();
    for (int i = 0; i < iters; ++i) {
        engine.add_order(make_bid(static_cast<std::uint64_t>(i) + 1,
                                  bid_price_for(static_cast<std::uint64_t>(i)), 100, ts),
                         OrderType::POST_ONLY);
    }

    mme::HdrPerformanceModule perf;
    auto wall0 = now();
    for (int i = 0; i < iters; ++i) {
        auto id = static_cast<std::uint64_t>(i) + 1;
        // Same price, qty down: the queue-position-preserving fast path.
        int new_qty = 50 + (i % 40);  // < 100 leaves, stays a shrink
        auto t0 = now();
        bool ok = engine.amend_order(id, bid_price_for(static_cast<std::uint64_t>(i)),
                                     new_qty, ts);
        auto t1 = now();
        perf.record_latency(ns_between(t0, t1));
        g_sink ^= ok ? 1 : 0;
    }
    perf.set_wall_time(now() - wall0);
    emit_section(csv, "matching_amend_order", perf);
}

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
            std::cout << "Usage: bench_matching [--iters N] [--report-path out.csv]\n";
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

    bench_add(iters, *csv);
    bench_cancel(iters, *csv);
    bench_amend(iters, *csv);

    // Force a use of the sink so it survives -O3 -flto.
    if (g_sink == 0x7fffffffffffffffLL) std::cerr << "sink\n";
    return 0;
}
