#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <streambuf>
#include <string>
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "include/SimulationConfig.h"
#include "include/HeuristicStrategy.h"
#include "include/Strategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"
#include "PerformanceModule.h"

namespace {
struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
};
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

    std::unique_ptr<Strategy> strategy;
    if (strategy_name == "heuristic") {
        strategy = std::make_unique<HeuristicStrategy>();
    } else if (strategy_name == "avellaneda-stoikov" || strategy_name == "as") {
        strategy = std::make_unique<AvellanedaStoikovStrategy>();
        strategy_name = "avellaneda-stoikov";
    } else {
        std::cerr << "Unknown strategy: " << strategy_name << "\n";
        return 1;
    }

    SimulationConfig config;
    config.seed = seed;
    config.iterations = events;
    config.latency_ms = 0;
    config.quiet = true;

    MarketSimulator simulator(config);
    RiskConfig risk_cfg;
    MarketMaker mm(simulator.instrument_meta(), risk_cfg, std::move(strategy));

    PerformanceModule perf(static_cast<size_t>(events));

    std::cout << "Strategy: " << strategy_name
              << ", events: " << events
              << ", seed: " << seed << "\n";

    // Silence MarketMaker hot-path prints (FILL/WARNING) inside the timed loop
    // so they don't inflate latency samples or pollute throughput.
    NullBuf null_buf;
    std::streambuf* prev_buf = std::cout.rdbuf(&null_buf);

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
        mm.on_market_data(md, simulator);
        auto t1 = std::chrono::steady_clock::now();

        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        perf.record_latency(ns);
        ++processed;
    }

    auto wall_end = std::chrono::steady_clock::now();
    perf.set_wall_time(wall_end - wall_start);

    std::cout.rdbuf(prev_buf);

    std::cout << "Benchmark complete: " << processed << " events processed\n";
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    std::cout << "Wall time: " << wall_ms << " ms\n\n";
    perf.report_latency_percentiles();

    return 0;
}
