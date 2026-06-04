#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "include/Logger.h"
#include "include/SimulationConfig.h"
#include "include/HeuristicStrategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"
#include "include/SpscLogger.h"

using namespace std;

std::atomic<bool> running{true};

// Signal handler must use only async-signal-safe functions (POSIX).
// std::cout / iostreams are not safe; use write(2) on a fixed buffer.
void signal_handler(int /*signal*/) {
    running.store(false, std::memory_order_relaxed);
    static const char msg[] = "\nReceived shutdown signal.\n";
    ssize_t r = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)r;
}

namespace {
const char* mode_to_string(SimulationMode mode) {
    switch (mode) {
        case SimulationMode::Simulate:
            return "simulate";
        case SimulationMode::Replay:
            return "replay";
        case SimulationMode::ItchReplay:
            return "itch_replay";
    }
    return "unknown";
}

SimulationMode parse_mode(const std::string& value) {
    if (value == "simulate") {
        return SimulationMode::Simulate;
    }
    if (value == "replay") {
        return SimulationMode::Replay;
    }
    if (value == "itch_replay") {
        return SimulationMode::ItchReplay;
    }
    throw std::invalid_argument(
        "Invalid --mode value: " + value +
        " (expected simulate|replay|itch_replay)");
}

mme::LatencyDistribution parse_latency_dist(const std::string& value) {
    if (value == "zero")        return mme::LatencyDistribution::Zero;
    if (value == "constant")    return mme::LatencyDistribution::Constant;
    if (value == "exponential") return mme::LatencyDistribution::Exponential;
    if (value == "lognormal")   return mme::LatencyDistribution::LogNormal;
    throw std::invalid_argument(
        "Invalid latency distribution: " + value +
        " (expected zero|constant|exponential|lognormal)");
}

void print_usage() {
    std::cout << "Usage: ./market_maker_simulator [options]\n"
              << "Options:\n"
              << "  --mode <name>       simulate|replay (default: simulate)\n"
              << "  --strategy <name>   heuristic|avellaneda-stoikov (default: heuristic)\n"
              << "  --seed <n>          RNG seed (default: 42)\n"
              << "  --iterations <n>    Number of events to process (default: 1000)\n"
              << "  --volatility <f>    Per-event mid noise stddev in dollars (default: 0.5)\n"
              << "  --spread <f>        Synthetic LOB spread in dollars (default: 0.1)\n"
              << "  --initial-price <f> Initial mid price in dollars (default: 100.0)\n"
              << "  --event-log <path>  Write generated MD events to binary log\n"
              << "  --replay <path>     Compatibility alias for --mode replay + replay path\n"
              << "  --engine-log <path> Route engine alerts through SPSC binary logger\n"
              << "                      (default: human-readable stdout output)\n"
              << "  --quiet             Suppress per-event output\n"
              << "  --help              Show this help text\n"
              << "\n"
              << "Per-stage latency (M8): all default to zero (byte-equal replay path).\n"
              << "  --feed-latency-dist       <zero|constant|exponential|lognormal>\n"
              << "  --feed-latency-mean-ns    <int>\n"
              << "  --feed-latency-stddev-ns  <int>  (LogNormal only)\n"
              << "  --ack-latency-dist        <...>\n"
              << "  --ack-latency-mean-ns     <int>\n"
              << "  --ack-latency-stddev-ns   <int>\n"
              << "  --matching-latency-dist   <...>\n"
              << "  --matching-latency-mean-ns   <int>\n"
              << "  --matching-latency-stddev-ns <int>\n"
              << "  --latency-seed            <uint>  RNG seed for latency draws (default: 0xC0FFEE)\n";
}

bool read_arg_value(int argc, char* argv[], int& i, std::string& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = argv[++i];
    return true;
}

std::string strategy_name = "heuristic";
std::string engine_log_path;

SimulationConfig parse_args(int argc, char* argv[]) {
    SimulationConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;

        if (arg == "--strategy") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--strategy requires a value");
            }
            if (value != "heuristic" && value != "avellaneda-stoikov") {
                throw std::invalid_argument("Invalid --strategy value: " + value + " (expected heuristic|avellaneda-stoikov)");
            }
            strategy_name = value;
        } else if (arg == "--seed") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--seed requires a value");
            }
            config.seed = static_cast<uint32_t>(std::stoul(value));
        } else if (arg == "--mode") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--mode requires a value");
            }
            config.mode = parse_mode(value);
        } else if (arg == "--iterations") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--iterations requires a value");
            }
            config.iterations = std::stoi(value);
        } else if (arg == "--volatility") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--volatility requires a value");
            }
            config.volatility = std::stod(value);
        } else if (arg == "--spread") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--spread requires a value");
            }
            config.spread = std::stod(value);
        } else if (arg == "--initial-price") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--initial-price requires a value");
            }
            config.initial_price = std::stod(value);
        } else if (arg == "--feed-latency-dist") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--feed-latency-dist requires a value");
            }
            config.feed_latency.kind = parse_latency_dist(value);
        } else if (arg == "--feed-latency-mean-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--feed-latency-mean-ns requires a value");
            }
            config.feed_latency.mean_ns = std::stoll(value);
        } else if (arg == "--feed-latency-stddev-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--feed-latency-stddev-ns requires a value");
            }
            config.feed_latency.stddev_ns = std::stoll(value);
        } else if (arg == "--ack-latency-dist") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--ack-latency-dist requires a value");
            }
            config.ack_latency.kind = parse_latency_dist(value);
        } else if (arg == "--ack-latency-mean-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--ack-latency-mean-ns requires a value");
            }
            config.ack_latency.mean_ns = std::stoll(value);
        } else if (arg == "--ack-latency-stddev-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--ack-latency-stddev-ns requires a value");
            }
            config.ack_latency.stddev_ns = std::stoll(value);
        } else if (arg == "--matching-latency-dist") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--matching-latency-dist requires a value");
            }
            config.matching_latency.kind = parse_latency_dist(value);
        } else if (arg == "--matching-latency-mean-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--matching-latency-mean-ns requires a value");
            }
            config.matching_latency.mean_ns = std::stoll(value);
        } else if (arg == "--matching-latency-stddev-ns") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--matching-latency-stddev-ns requires a value");
            }
            config.matching_latency.stddev_ns = std::stoll(value);
        } else if (arg == "--latency-seed") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--latency-seed requires a value");
            }
            config.latency_seed = static_cast<std::uint32_t>(std::stoul(value));
        } else if (arg == "--event-log") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--event-log requires a value");
            }
            config.event_log_path = value;
        } else if (arg == "--replay") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--replay requires a value");
            }
            config.replay_log_path = value;
            config.mode = SimulationMode::Replay;
        } else if (arg == "--engine-log") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--engine-log requires a value");
            }
            engine_log_path = value;
        } else if (arg == "--quiet") {
            config.quiet = true;
        } else if (arg == "--help") {
            print_usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown argument: " + arg);
        }
    }
    return config;
}

uint64_t update_fnv1a(uint64_t hash, const std::string& data) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (unsigned char ch : data) {
        hash ^= ch;
        hash *= kPrime;
    }
    return hash;
}
} // namespace

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    SimulationConfig config;
    try {
        config = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n\n";
        print_usage();
        return 1;
    }

    if (config.iterations <= 0) {
        std::cerr << "--iterations must be > 0\n";
        return 1;
    }
    auto validate_stage = [](const char* name, const mme::StageLatencyConfig& s) -> bool {
        if (s.mean_ns < 0) {
            std::cerr << "--" << name << "-latency-mean-ns must be >= 0\n";
            return false;
        }
        if (s.stddev_ns < 0) {
            std::cerr << "--" << name << "-latency-stddev-ns must be >= 0\n";
            return false;
        }
        if ((s.kind == mme::LatencyDistribution::Exponential ||
             s.kind == mme::LatencyDistribution::LogNormal) && s.mean_ns == 0) {
            std::cerr << "--" << name << "-latency-dist requires --"
                      << name << "-latency-mean-ns > 0\n";
            return false;
        }
        return true;
    };
    if (!validate_stage("feed",     config.feed_latency))     return 1;
    if (!validate_stage("ack",      config.ack_latency))      return 1;
    if (!validate_stage("matching", config.matching_latency)) return 1;
    if (config.mode == SimulationMode::Replay && config.replay_log_path.empty()) {
        std::cerr << "--mode replay requires --replay <path>\n";
        return 1;
    }
    if (config.mode == SimulationMode::Replay && !config.event_log_path.empty()) {
        std::cerr << "--event-log cannot be used with --mode replay\n";
        return 1;
    }
    if (config.mode == SimulationMode::Simulate && !config.replay_log_path.empty()) {
        std::cerr << "--replay provided while mode is simulate; use --mode replay\n";
        return 1;
    }

    try {
        MarketSimulator simulator(config);

        std::unique_ptr<Strategy> strategy;
        if (strategy_name == "avellaneda-stoikov") {
            strategy = std::make_unique<AvellanedaStoikovStrategy>();
        } else {
            strategy = std::make_unique<HeuristicStrategy>();
        }
        RiskConfig risk_cfg;
        const Instrument instrument = simulator.instrument_meta();
        std::unique_ptr<Logger> mm_logger;
        if (!engine_log_path.empty()) {
            mm_logger = std::make_unique<SpscLogger>(engine_log_path);
        } else {
            mm_logger = std::make_unique<StdoutLogger>();
        }
        MarketMaker mm(instrument, risk_cfg, std::move(strategy),
                       std::move(mm_logger));

        int processed = 0;
        int64_t last_sequence = 0;
        double sum_bid = 0.0;
        double sum_ask = 0.0;
        int64_t total_trade_volume = 0;
        int64_t total_partial_fill_volume = 0;
        int64_t total_mm_fill_volume = 0;
        int64_t total_mm_fill_count = 0;
        uint64_t checksum = 1469598103934665603ULL;

        while (running && processed < config.iterations) {
            MarketDataEvent md;
            try {
                md = simulator.generate_event();
            } catch (const std::out_of_range&) {
                break;
            }

            // MM reads market data, submits/cancels orders via simulator
            mm.on_market_data(md, simulator);

            ++processed;
            last_sequence = md.sequence_number;
            const double bid_dollars = instrument.to_price(md.best_bid_price);
            const double ask_dollars = instrument.to_price(md.best_ask_price);
            sum_bid += bid_dollars;
            sum_ask += ask_dollars;

            std::ostringstream event_fp;
            event_fp << md.sequence_number << "|"
                     << std::fixed << std::setprecision(6)
                     << bid_dollars << "|"
                     << ask_dollars << "|"
                     << md.best_bid_size << "|"
                     << md.best_ask_size;

            for (const auto& trade : md.trades) {
                total_trade_volume += trade.size;
                event_fp << "|T:" << (trade.aggressor_side == Side::BUY ? "BUY" : "SELL")
                         << ":" << std::fixed << std::setprecision(6)
                         << instrument.to_price(trade.price) << ":" << trade.size;
            }
            for (const auto& fill : md.partial_fills) {
                total_partial_fill_volume += fill.filled_size;
                event_fp << "|F:" << fill.order_id << ":" << std::fixed << std::setprecision(6)
                         << instrument.to_price(fill.price) << ":" << fill.filled_size << ":" << fill.remaining_size;
            }
            for (const auto& fill : md.mm_fills) {
                total_mm_fill_volume += fill.fill_qty;
                ++total_mm_fill_count;
            }
            checksum = update_fnv1a(checksum, event_fp.str());

            if (!config.quiet && (processed <= 5 || processed % 100 == 0)) {
                std::cout << "Event " << md.sequence_number
                          << " bid=" << std::fixed << std::setprecision(4) << bid_dollars
                          << " ask=" << ask_dollars
                          << " trades=" << md.trades.size()
                          << " mm_fills=" << md.mm_fills.size() << "\n";
            }
        }

        const double avg_bid = processed == 0 ? 0.0 : (sum_bid / processed);
        const double avg_ask = processed == 0 ? 0.0 : (sum_ask / processed);
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "SUMMARY"
                  << " mode=" << mode_to_string(config.mode)
                  << " seed=" << config.seed
                  << " iterations=" << config.iterations
                  << " processed=" << processed
                  << " last_sequence=" << last_sequence
                  << " avg_bid=" << avg_bid
                  << " avg_ask=" << avg_ask
                  << " trade_volume=" << total_trade_volume
                  << " partial_fill_volume=" << total_partial_fill_volume
                  << " mm_fill_count=" << total_mm_fill_count
                  << " mm_fill_volume=" << total_mm_fill_volume
                  << " checksum=" << checksum
                  << "\n";

        mm.report();

        if (processed == 0) {
            std::cerr << "No events processed.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Simulation failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Simulation failed with unknown error." << std::endl;
        return 1;
    }

    return 0;
}
