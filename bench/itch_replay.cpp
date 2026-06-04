// M10/5 end-to-end ITCH replay backtest runner.
//
// Loads a Nasdaq TotalView-ITCH 5.0 binary tape, drives the matching
// engine off the tape with one symbol filtered in, runs the MM strategy
// against the resulting market data, and emits a CSV backtest report
// (PnL trace, fill log, inventory trace, summary). Stderr carries a
// short progress summary.
//
// Usage:
//   ./itch_replay --tape <path> --symbol AAPL \
//                 [--max-events N] [--report-path out.csv] [--seed 42]
//
// CSV sections (mirrors the bench_lob_realism convention):
//   # section: pnl_trace
//     ts_ns,total_pnl,mid
//   # section: fill_log
//     ts_ns,side,price,qty
//   # section: inventory_trace
//     ts_ns,inventory
//   # section: summary
//     metric,value
//
// The report file path defaults to stdout if --report-path is omitted.

#include "MarketSimulator.h"
#include "include/BacktestMetrics.h"
#include "include/HeuristicStrategy.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>

namespace {

struct Args {
    std::string   tape_path;
    std::string   symbol;
    std::string   report_path;          // empty → stdout
    int           max_events = 0;       // 0 = run until tape exhausted
    std::uint32_t seed       = 42;
};

[[noreturn]] void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog
        << " --tape <path> --symbol <8-char> "
        << "[--max-events N] [--report-path out.csv] [--seed 42]\n";
    std::exit(2);
}

Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto take = [&]() -> std::string {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if      (arg == "--tape")        a.tape_path   = take();
        else if (arg == "--symbol")      a.symbol      = take();
        else if (arg == "--report-path") a.report_path = take();
        else if (arg == "--max-events")  a.max_events  = std::stoi(take());
        else if (arg == "--seed")        a.seed        =
            static_cast<std::uint32_t>(std::stoul(take()));
        else                              usage(argv[0]);
    }
    if (a.tape_path.empty() || a.symbol.empty()) usage(argv[0]);
    return a;
}

void emit_report(std::ostream& out, const mme::BacktestMetrics& bm,
                 double final_pnl, int final_inventory,
                 double tick_size, std::int64_t events_processed) {
    out << "# section: pnl_trace\n";
    out << "ts_ns,total_pnl,mid\n";
    const auto& pnl = bm.pnl_trace();
    const auto& mid = bm.mid_trace();
    const std::size_t n = std::min(pnl.size(), mid.size());
    for (std::size_t i = 0; i < n; ++i) {
        out << pnl[i].ts_ns << ',' << pnl[i].pnl << ',' << mid[i].mid << '\n';
    }

    out << "# section: fill_log\n";
    out << "ts_ns,side,price,qty\n";
    for (const auto& f : bm.fills()) {
        out << f.ts_ns << ','
            << (f.side == Side::BUY ? "BUY" : "SELL") << ','
            << f.price << ',' << f.qty << '\n';
    }

    out << "# section: inventory_trace\n";
    out << "ts_ns,inventory\n";
    for (const auto& s : bm.inventory_trace()) {
        out << s.ts_ns << ',' << s.pnl << '\n';
    }

    out << "# section: summary\n";
    out << "metric,value\n";
    out << "events_processed,"      << events_processed                << '\n';
    out << "final_total_pnl,"       << final_pnl                       << '\n';
    out << "final_inventory,"       << final_inventory                 << '\n';
    out << "fills_count,"           << bm.fills_count()                << '\n';
    out << "quotes_posted,"         << bm.quotes_posted()              << '\n';
    out << "fill_rate,"             << bm.fill_rate()                  << '\n';
    out << "max_drawdown,"          << bm.max_drawdown()               << '\n';
    out << "sharpe_annualized,"     << bm.sharpe_annualized()          << '\n';
    out << "adv_sel_100ms_dollars," << bm.avg_adverse_selection(100'000'000) << '\n';
    out << "adv_sel_1s_dollars,"    << bm.avg_adverse_selection(1'000'000'000) << '\n';
    out << "tick_size,"             << tick_size                       << '\n';
}

}  // namespace

int main(int argc, char* argv[]) try {
    const Args args = parse_args(argc, argv);

    SimulationConfig cfg;
    cfg.instrument    = args.symbol;
    cfg.tick_size     = 0.01;
    cfg.mode          = SimulationMode::ItchReplay;
    cfg.itch_log_path = args.tape_path;
    cfg.itch_symbol   = args.symbol;
    cfg.seed          = args.seed;
    cfg.quiet         = true;

    MarketSimulator simulator(cfg);

    HeuristicStrategy strategy;
    NullLogger        logger;
    RiskConfig        risk_cfg;
    mme::MarketMakerT<HeuristicStrategy, NullLogger> mm(
        simulator.instrument_meta(), risk_cfg, &strategy, &logger);

    mme::BacktestMetrics metrics;

    // 1-second cadence for pnl sampling (matches BacktestMetrics default).
    constexpr std::int64_t kPnlSampleIntervalNs   = 1'000'000'000;
    constexpr std::int64_t kInventoryCadenceNs    = 100'000'000;
    std::int64_t next_pnl_sample_ns       = 0;
    std::int64_t next_inventory_sample_ns = 0;
    std::int64_t event_clock_ns           = 0;

    int prev_fills = 0;
    int loop_count = 0;
    while (true) {
        if (args.max_events > 0 && loop_count >= args.max_events) break;

        MarketDataEvent md;
        try {
            md = simulator.generate_event();
        } catch (const std::out_of_range&) {
            break;
        }
        ++loop_count;

        // Derive a stable event-clock from ITCH timestamps would be
        // cleaner, but generate_event() drives current_time() forward
        // by 1 ms per call internally; we synthesize an analogous
        // monotonic clock here at the same cadence so metric windows
        // (100 ms / 1 s) are interpretable. The mid trace records the
        // engine-derived mid at each step.
        event_clock_ns += 1'000'000;  // 1 ms per event

        // Mid price for marking + drift lookback.
        double mid_dollars = 0.0;
        if (md.best_bid_price > 0 && md.best_ask_price > 0) {
            const double bid_d = simulator.instrument_meta().to_price(md.best_bid_price);
            const double ask_d = simulator.instrument_meta().to_price(md.best_ask_price);
            mid_dollars = 0.5 * (bid_d + ask_d);
        }
        metrics.record_mid(event_clock_ns, mid_dollars);

        // Capture MM fills before tick_once so we can attribute them
        // with the pre-quote inventory snapshot.
        const auto fills_this_event = md.mm_fills;

        mm.on_market_data(md, simulator);

        // Every on_market_data call where MM has live quotes counts as
        // a quote post for the fill-rate denominator. We approximate
        // by counting one quote per side per non-empty book event.
        if (md.best_bid_price > 0) metrics.record_quote_posted();
        if (md.best_ask_price > 0) metrics.record_quote_posted();

        for (const auto& f : fills_this_event) {
            const double px_dollars = simulator.instrument_meta().to_price(f.price);
            metrics.record_fill(event_clock_ns, f.side, px_dollars, f.fill_qty);
        }

        if (event_clock_ns >= next_pnl_sample_ns) {
            metrics.record_pnl(event_clock_ns, mm.get_total_pnl());
            next_pnl_sample_ns = event_clock_ns + kPnlSampleIntervalNs;
        }
        if (event_clock_ns >= next_inventory_sample_ns) {
            metrics.record_inventory(event_clock_ns, mm.get_inventory());
            next_inventory_sample_ns = event_clock_ns + kInventoryCadenceNs;
        }

        const int now_fills = mm.get_total_fills();
        if (now_fills != prev_fills) {
            prev_fills = now_fills;
        }
    }

    // Final PnL sample at the closing tick.
    metrics.record_pnl(event_clock_ns, mm.get_total_pnl());
    metrics.record_inventory(event_clock_ns, mm.get_inventory());

    std::cerr << "ITCH replay complete:\n"
              << "  events_processed:    " << loop_count               << "\n"
              << "  fills_count:         " << metrics.fills_count()    << "\n"
              << "  quotes_posted:       " << metrics.quotes_posted()  << "\n"
              << "  fill_rate:           " << metrics.fill_rate()      << "\n"
              << "  final_total_pnl:     $" << mm.get_total_pnl()      << "\n"
              << "  final_inventory:     " << mm.get_inventory()       << " shares\n"
              << "  max_drawdown:        $" << metrics.max_drawdown()  << "\n"
              << "  sharpe_annualized:   " << metrics.sharpe_annualized() << "\n"
              << "  adv_sel_100ms:       $" << metrics.avg_adverse_selection(100'000'000) << "\n"
              << "  adv_sel_1s:          $" << metrics.avg_adverse_selection(1'000'000'000) << "\n";

    if (args.report_path.empty()) {
        emit_report(std::cout, metrics, mm.get_total_pnl(),
                    mm.get_inventory(), cfg.tick_size, loop_count);
    } else {
        std::ofstream out(args.report_path);
        if (!out) {
            std::cerr << "ERROR: failed to open report path: "
                      << args.report_path << "\n";
            return 1;
        }
        emit_report(out, metrics, mm.get_total_pnl(),
                    mm.get_inventory(), cfg.tick_size, loop_count);
    }
    return 0;
}
catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
}
