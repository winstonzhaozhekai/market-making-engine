// M9/5 LOB realism benchmarks. Emits three CSV sections to stdout, each
// preceded by a `# section: …` comment line so consumers can split with
// awk / pandas. A short progress summary lands on stderr.
//
//   # section: spread_distribution
//     spread_ticks,count,frequency
//   # section: top_of_book_bid_depth
//     bid_size,count,frequency
//   # section: fill_rate_sweep
//     feed_latency_mean_ns,fills,fills_per_1k_events
//
// The first two sections use a shallow-book HLR calibration (level-0
// depletion occurs occasionally → spread varies, depth fluctuates).
// The third uses a deep-book calibration where MM's stale quote at the
// (still-current) inside joins synthetic FIFO time priority, which is
// the regime where the M9 done-when #3 monotone-from-zero claim holds.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "MarketSimulator.h"
#include "include/HeuristicStrategy.h"
#include "include/Logger.h"
#include "include/MarketMakerT.h"
#include "include/SimulationConfig.h"

namespace {

struct Args {
    int           iterations    = 100'000;
    int           realism_iters = 50'000;
    std::uint32_t seed          = 42;
    std::uint32_t lob_seed      = 0xB00B5u;
    std::uint32_t latency_seed  = 0xC0FFEEu;
    std::vector<std::int64_t> sweep_ns = {
        0, 50'000, 100'000, 500'000, 2'000'000
    };
};

Args parse_args(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto take = [&](const char* name) {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    std::string(name) + " requires a value");
            }
            return std::string(argv[++i]);
        };
        if      (arg == "--iterations")    a.iterations   = std::stoi(take("--iterations"));
        else if (arg == "--realism-iters") a.realism_iters = std::stoi(take("--realism-iters"));
        else if (arg == "--seed")          a.seed         = static_cast<std::uint32_t>(std::stoul(take("--seed")));
        else if (arg == "--lob-seed")      a.lob_seed     = static_cast<std::uint32_t>(std::stoul(take("--lob-seed")));
        else if (arg == "--latency-seed")  a.latency_seed = static_cast<std::uint32_t>(std::stoul(take("--latency-seed")));
        else if (arg == "--sweep") {
            a.sweep_ns.clear();
            std::string csv = take("--sweep");
            std::string tok;
            for (char c : csv) {
                if (c == ',') {
                    if (!tok.empty()) a.sweep_ns.push_back(std::stoll(tok));
                    tok.clear();
                } else tok.push_back(c);
            }
            if (!tok.empty()) a.sweep_ns.push_back(std::stoll(tok));
        } else if (arg == "--help") {
            std::cout
                << "Usage: bench_lob_realism [options]\n"
                << "  --iterations <n>      events per fill-rate sweep point (default: 100000)\n"
                << "  --realism-iters <n>   events for spread + depth scans (default: 50000)\n"
                << "  --seed <n>            sim seed (default: 42)\n"
                << "  --lob-seed <n>        HLR RNG seed (default: 0xB00B5)\n"
                << "  --latency-seed <n>    latency RNG seed (default: 0xC0FFEE)\n"
                << "  --sweep <csv-ns>      feed-latency sweep ns (default: 0,50k,100k,500k,2M)\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }
    return a;
}

SimulationConfig shallow_book_config(const Args& a) {
    SimulationConfig cfg;
    cfg.seed       = a.seed;
    cfg.lob_seed   = a.lob_seed;
    cfg.iterations = a.realism_iters;
    cfg.quiet      = true;
    cfg.lob_model  = LobModel::QueueReactive;
    // Shallow-book: q*_0 ≈ 80 units → level-0 depletion is a non-rare
    // event → spread + top-of-book depth vary measurably.
    cfg.hlr.lambda_per_ns      = {{1.5e-5, 1.0e-5, 7e-6, 5e-6, 4e-6}};
    cfg.hlr.mu_per_unit_per_ns = 5.0e-7;
    cfg.hlr.theta_per_ns       = 1.0e-5;
    cfg.hlr.initial_queue      = {{50, 60, 70, 70, 70}};
    return cfg;
}

SimulationConfig deep_book_config(const Args& a, std::int64_t feed_mean_ns) {
    SimulationConfig cfg;
    cfg.seed       = a.seed;
    cfg.lob_seed   = a.lob_seed;
    cfg.iterations = a.iterations;
    cfg.quiet      = true;
    cfg.lob_model  = LobModel::QueueReactive;
    // Deep-book: q*_0 ≈ 400 units → depletion rare in the feed_latency
    // sweep window → MM's stale quotes join synthetic FIFO behind time
    // priority → adverse-selection bump suppressed.
    cfg.hlr.lambda_per_ns      = {{5e-5, 2.5e-5, 1.7e-5, 1.2e-5, 1.0e-5}};
    cfg.hlr.mu_per_unit_per_ns = 1.0e-7;
    cfg.hlr.theta_per_ns       = 1.0e-5;
    cfg.hlr.initial_queue      = {{400, 400, 400, 400, 400}};
    cfg.latency_seed           = a.latency_seed;
    cfg.feed_latency.kind      = feed_mean_ns == 0
        ? mme::LatencyDistribution::Zero
        : mme::LatencyDistribution::Constant;
    cfg.feed_latency.mean_ns   = feed_mean_ns;
    return cfg;
}

void emit_distribution_csv(const std::map<int, int>& hist, int total) {
    for (const auto& [bucket, count] : hist) {
        const double freq = static_cast<double>(count) /
                            static_cast<double>(total);
        std::cout << bucket << "," << count << "," << freq << "\n";
    }
}

void run_spread_section(const Args& args) {
    auto cfg = shallow_book_config(args);
    MarketSimulator sim(cfg);
    std::map<int, int> hist;
    int total = 0;
    for (int i = 0; i < cfg.iterations; ++i) {
        auto md = sim.generate_event();
        if (md.best_bid_price > Ticks{0} && md.best_ask_price > Ticks{0}) {
            const int s = static_cast<int>(md.best_ask_price - md.best_bid_price);
            ++hist[s];
            ++total;
        }
    }
    std::cout << "# section: spread_distribution\n";
    std::cout << "spread_ticks,count,frequency\n";
    emit_distribution_csv(hist, total);
    std::cerr << "[spread]   " << total << " samples across "
              << hist.size() << " distinct spread values\n";
}

void run_depth_section(const Args& args) {
    auto cfg = shallow_book_config(args);
    MarketSimulator sim(cfg);
    std::map<int, int> hist;
    int total = 0;
    for (int i = 0; i < cfg.iterations; ++i) {
        auto md = sim.generate_event();
        ++hist[md.best_bid_size];
        ++total;
    }
    std::cout << "# section: top_of_book_bid_depth\n";
    std::cout << "bid_size,count,frequency\n";
    emit_distribution_csv(hist, total);
    std::cerr << "[depth]    " << total << " samples across "
              << hist.size() << " distinct depths\n";
}

std::int64_t fills_at_feed_latency(const Args& args, std::int64_t feed_mean_ns) {
    auto cfg = deep_book_config(args, feed_mean_ns);
    MarketSimulator sim(cfg);
    HeuristicStrategy strategy;
    NullLogger logger;
    mme::MarketMakerT<HeuristicStrategy, NullLogger> mm(
        sim.instrument_meta(), RiskConfig{}, &strategy, &logger);
    for (int i = 0; i < cfg.iterations; ++i) {
        MarketDataEvent md;
        try { md = sim.generate_event(); }
        catch (const std::out_of_range&) { break; }
        mm.on_market_data(md, sim);
    }
    return static_cast<std::int64_t>(mm.get_total_fills());
}

void run_fill_rate_section(const Args& args) {
    std::cout << "# section: fill_rate_sweep\n";
    std::cout << "feed_latency_mean_ns,fills,fills_per_1k_events\n";
    for (auto ns : args.sweep_ns) {
        const auto fills = fills_at_feed_latency(args, ns);
        const double per_1k = 1000.0 * static_cast<double>(fills) /
                              static_cast<double>(args.iterations);
        std::cout << ns << "," << fills << "," << per_1k << "\n";
        std::cerr << "[fills]    feed_latency=" << ns
                  << "ns  fills=" << fills
                  << "  per_1k=" << per_1k << "\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "argument error: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "bench_lob_realism: seed=" << args.seed
              << " lob_seed=0x" << std::hex << args.lob_seed << std::dec
              << " realism_iters=" << args.realism_iters
              << " sweep_iters=" << args.iterations << "\n";

    const auto t0 = std::chrono::steady_clock::now();
    run_spread_section(args);
    std::cout << "\n";
    run_depth_section(args);
    std::cout << "\n";
    run_fill_rate_section(args);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t1 - t0).count();
    std::cerr << "bench_lob_realism complete in " << ms << " ms\n";
    return 0;
}
