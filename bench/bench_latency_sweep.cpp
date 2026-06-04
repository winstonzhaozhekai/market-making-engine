// Fill-rate sweep across feed latency values, holding everything else
// (seed, volatility, iterations, strategy) fixed. CSV emitted to stdout so
// the output pipes cleanly into analysis tools; a tabular summary lands on
// stderr for at-a-glance reading.
//
// CSV schema (stdout):
//   feed_latency_mean_ns,fills,fills_per_1k_events
//
// Use case: validate the M8 done-when by sweeping `--feed-latency-mean-ns`
// and showing the monotone decline that real-exchange backtests display
// once the adverse-selection regime is cleared (M8/3 queue-position gate +
// volatility chosen so drift-over-lag is comparable to the synthetic LOB
// spread).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "MarketSimulator.h"
#include "include/HeuristicStrategy.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"
#include "strategies/AvellanedaStoikovStrategy.h"

namespace {

template <class StrategyT>
std::int64_t run_one_sweep_point(const SimulationConfig& cfg,
                                  StrategyT& strategy) {
    MarketSimulator simulator(cfg);
    RiskConfig risk_cfg;
    NullLogger logger;
    mme::MarketMakerT<StrategyT, NullLogger> mm(
        simulator.instrument_meta(), risk_cfg, &strategy, &logger);

    for (int i = 0; i < cfg.iterations; ++i) {
        MarketDataEvent md;
        try {
            md = simulator.generate_event();
        } catch (const std::out_of_range&) {
            break;
        }
        mm.on_market_data(md, simulator);
    }
    return static_cast<std::int64_t>(mm.get_total_fills());
}

struct Args {
    int                       iterations  = 100000;
    std::uint32_t             seed        = 42;
    std::uint32_t             latency_seed = 0xC0FFEEu;
    double                    volatility  = 0.005;
    std::string               strategy    = "heuristic";
    std::vector<std::int64_t> sweep_ns    = {
        0, 10'000, 50'000, 100'000, 200'000,
        500'000, 1'000'000, 2'000'000, 5'000'000
    };
};

Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto take = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string(name) + " requires a value");
            }
            return std::string(argv[++i]);
        };
        if (arg == "--iterations")        a.iterations   = std::stoi(take("--iterations"));
        else if (arg == "--seed")         a.seed         = static_cast<std::uint32_t>(std::stoul(take("--seed")));
        else if (arg == "--latency-seed") a.latency_seed = static_cast<std::uint32_t>(std::stoul(take("--latency-seed")));
        else if (arg == "--volatility")   a.volatility   = std::stod(take("--volatility"));
        else if (arg == "--strategy")     a.strategy     = take("--strategy");
        else if (arg == "--sweep") {
            a.sweep_ns.clear();
            std::stringstream ss(take("--sweep"));
            std::string token;
            while (std::getline(ss, token, ',')) {
                a.sweep_ns.push_back(std::stoll(token));
            }
        } else if (arg == "--help") {
            std::cout
                << "Usage: bench_latency_sweep [options]\n"
                << "  --iterations <n>     events per sweep point (default: 100000)\n"
                << "  --seed <n>           sim RNG seed (default: 42)\n"
                << "  --latency-seed <n>   latency draws seed (default: 0xC0FFEE)\n"
                << "  --volatility <f>     per-event mid stddev in dollars (default: 0.005)\n"
                << "  --strategy <name>    heuristic|avellaneda-stoikov (default: heuristic)\n"
                << "  --sweep <csv-ns>     comma-separated feed_latency_mean_ns values\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }
    return a;
}

void build_config(SimulationConfig& cfg, const Args& a, std::int64_t mean_ns) {
    cfg.seed = a.seed;
    cfg.iterations = a.iterations;
    cfg.volatility = a.volatility;
    cfg.quiet = true;
    cfg.latency_seed = a.latency_seed;
    // The M8 monotone fill-rate-vs-feed-latency story is a brownian-
    // generator + 20%-IOC-counterparty property. Under QueueReactive the
    // curve is flat (synthetic FIFO time priority absorbs the aggressor
    // regardless of MM staleness). Pin to Legacy so this sweep reports
    // the M8 narrative; use `bench_lob_realism` for QueueReactive sweeps.
    cfg.lob_model = LobModel::Legacy;
    if (mean_ns == 0) {
        cfg.feed_latency.kind    = mme::LatencyDistribution::Zero;
        cfg.feed_latency.mean_ns = 0;
    } else {
        cfg.feed_latency.kind    = mme::LatencyDistribution::Constant;
        cfg.feed_latency.mean_ns = mean_ns;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "Sweep: strategy=" << args.strategy
              << " seed=" << args.seed
              << " volatility=" << args.volatility
              << " iterations=" << args.iterations << "\n";
    std::cerr << "  feed_latency_mean_ns      fills    fills/1k_events\n";

    std::cout << "feed_latency_mean_ns,fills,fills_per_1k_events\n";

    const auto wall_start = std::chrono::steady_clock::now();
    for (std::int64_t mean_ns : args.sweep_ns) {
        SimulationConfig cfg;
        build_config(cfg, args, mean_ns);

        std::int64_t fills = 0;
        if (args.strategy == "heuristic") {
            HeuristicStrategy strategy;
            fills = run_one_sweep_point(cfg, strategy);
        } else if (args.strategy == "avellaneda-stoikov" || args.strategy == "as") {
            AvellanedaStoikovStrategy strategy;
            fills = run_one_sweep_point(cfg, strategy);
        } else {
            std::cerr << "unknown strategy: " << args.strategy << "\n";
            return 1;
        }

        const double per_1k = 1000.0 * static_cast<double>(fills)
                            / static_cast<double>(args.iterations);
        std::cout << mean_ns << "," << fills << "," << per_1k << "\n";
        std::cerr << "  " << std::right;
        std::cerr.width(20); std::cerr << mean_ns;
        std::cerr.width(10); std::cerr << fills;
        std::cerr.width(20); std::cerr << per_1k << "\n";
    }
    const auto wall_end = std::chrono::steady_clock::now();
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - wall_start).count();
    std::cerr << "Sweep complete in " << wall_ms << " ms.\n";

    return 0;
}
