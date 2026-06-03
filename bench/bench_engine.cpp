#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include "MarketSimulator.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"
#include "include/HeuristicStrategy.h"
#include "include/Strategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"
#include "PerformanceModule.h"

// Templated on the concrete strategy + logger so the compiler can
// devirtualize compute_quotes / on_fill / on_sequence_gap / on_empty_book /
// on_amend_rejected. `[[gnu::noinline]]` keeps the tick function as a
// discoverable symbol under -O3 -flto so the M7 no-virtual-dispatch
// disassembly check has something to point at. Without it the whole loop
// gets inlined into main and we lose the symbol anchor.
template <class StrategyT, class LoggerT>
[[gnu::noinline]] static void tick_once(
    mme::MarketMakerT<StrategyT, LoggerT>& mm,
    const MarketDataEvent& md,
    MarketSimulator& simulator) {
    mm.on_market_data(md, simulator);
}

template <class StrategyT, class LoggerT>
static void run_bench(int events, uint32_t seed,
                      const std::string& strategy_name,
                      StrategyT& strategy, LoggerT& logger) {
    SimulationConfig config;
    config.seed = seed;
    config.iterations = events;
    config.quiet = true;

    MarketSimulator simulator(config);
    RiskConfig risk_cfg;
    mme::MarketMakerT<StrategyT, LoggerT> mm(
        simulator.instrument_meta(), risk_cfg, &strategy, &logger);

    PerformanceModule perf(static_cast<size_t>(events));

    std::cout << "Strategy: " << strategy_name
              << ", events: " << events
              << ", seed: " << seed << "\n";

    auto wall_start = std::chrono::steady_clock::now();
    int processed = 0;

    for (int i = 0; i < events; ++i) {
        MarketDataEvent md;
        try {
            md = simulator.generate_event();
        } catch (const std::out_of_range&) {
            break;
        }

        auto t0 = std::chrono::steady_clock::now();
        tick_once(mm, md, simulator);
        auto t1 = std::chrono::steady_clock::now();

        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        perf.record_latency(ns);
        ++processed;
    }

    auto wall_end = std::chrono::steady_clock::now();
    perf.set_wall_time(wall_end - wall_start);

    std::cout << "Benchmark complete: " << processed << " events processed\n";
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    std::cout << "Wall time: " << wall_ms << " ms\n\n";
    perf.report_latency_percentiles();
}

int main(int argc, char* argv[]) {
    int events = 10000;
    uint32_t seed = 42;
    std::string strategy_name = "heuristic";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--events" && i + 1 < argc) {
            events = std::stoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--strategy" && i + 1 < argc) {
            strategy_name = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: bench_engine [--events N] [--seed N] "
                         "[--strategy heuristic|avellaneda-stoikov]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 1;
        }
    }

    NullLogger logger;
    if (strategy_name == "heuristic") {
        HeuristicStrategy strategy;
        run_bench(events, seed, strategy_name, strategy, logger);
    } else if (strategy_name == "avellaneda-stoikov" || strategy_name == "as") {
        AvellanedaStoikovStrategy strategy;
        run_bench(events, seed, "avellaneda-stoikov", strategy, logger);
    } else {
        std::cerr << "Unknown strategy: " << strategy_name << "\n";
        return 1;
    }

    return 0;
}
