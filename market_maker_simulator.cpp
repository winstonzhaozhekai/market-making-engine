#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "include/SimulationConfig.h"
#include "include/HeuristicStrategy.h"
#include "strategies/AvellanedaStoikovStrategy.h"
#include "include/BinaryLogger.h"

using namespace std;

volatile bool running = true;

void signal_handler(int signal) {
    running = false;
    std::cout << "\nReceived signal " << signal << ", shutting down.\n";
}

namespace {
const char* mode_to_string(SimulationMode mode) {
    switch (mode) {
        case SimulationMode::Simulate:
            return "simulate";
        case SimulationMode::Replay:
            return "replay";
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
    throw std::invalid_argument("Invalid --mode value: " + value + " (expected simulate|replay)");
}

void print_usage() {
    std::cout << "Usage: ./market_maker_simulator [options]\n"
              << "Options:\n"
              << "  --mode <name>       simulate|replay (default: simulate)\n"
              << "  --strategy <name>   heuristic|avellaneda-stoikov (default: heuristic)\n"
              << "  --seed <n>          RNG seed (default: 42)\n"
              << "  --iterations <n>    Number of events to process (default: 1000)\n"
              << "  --latency-ms <n>    Per-event latency in ms (default: 10)\n"
              << "  --event-log <path>  Write generated events to log file\n"
              << "  --replay <path>     Compatibility alias for --mode replay + replay path\n"
              << "  --binary-log <path> Write events in compact binary format\n"
              << "  --quiet             Suppress per-event output\n"
              << "  --help              Show this help text\n";
}

bool read_arg_value(int argc, char* argv[], int& i, std::string& out) {
    if (i + 1 >= argc) {
        return false;
    }
    out = argv[++i];
    return true;
}

std::string strategy_name = "heuristic";
std::string binary_log_path;

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
        } else if (arg == "--latency-ms") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--latency-ms requires a value");
            }
            config.latency_ms = std::stoi(value);
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
        } else if (arg == "--binary-log") {
            if (!read_arg_value(argc, argv, i, value)) {
                throw std::invalid_argument("--binary-log requires a value");
            }
            binary_log_path = value;
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
    if (config.latency_ms < 0) {
        std::cerr << "--latency-ms must be >= 0\n";
        return 1;
    }
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
        MarketMaker mm(risk_cfg, std::move(strategy));

        // Optional binary logger
        std::unique_ptr<BinaryLogger> bin_logger;
        if (!binary_log_path.empty()) {
            bin_logger = std::make_unique<BinaryLogger>(binary_log_path);
            if (!bin_logger->is_open()) {
                std::cerr << "Failed to open binary log: " << binary_log_path << "\n";
                return 1;
            }
        }

        int processed = 0;
        int64_t last_sequence = 0;
        double sum_bid = 0.0;
        double sum_ask = 0.0;
        int64_t total_trade_volume = 0;
        int64_t total_partial_fill_volume = 0;
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

            // Binary log if enabled
            if (bin_logger) {
                bin_logger->log_event(md);
            }

            ++processed;
            last_sequence = md.sequence_number;
            sum_bid += md.best_bid_price;
            sum_ask += md.best_ask_price;

            std::ostringstream event_fp;
            event_fp << md.sequence_number << "|"
                     << std::fixed << std::setprecision(6)
                     << md.best_bid_price << "|"
                     << md.best_ask_price << "|"
                     << md.best_bid_size << "|"
                     << md.best_ask_size;

            for (const auto& trade : md.trades) {
                total_trade_volume += trade.size;
                event_fp << "|T:" << (trade.aggressor_side == Side::BUY ? "BUY" : "SELL")
                         << ":" << std::fixed << std::setprecision(6)
                         << trade.price << ":" << trade.size;
            }
            for (const auto& fill : md.partial_fills) {
                total_partial_fill_volume += fill.filled_size;
                event_fp << "|F:" << fill.order_id << ":" << std::fixed << std::setprecision(6)
                         << fill.price << ":" << fill.filled_size << ":" << fill.remaining_size;
            }
            checksum = update_fnv1a(checksum, event_fp.str());

            if (!config.quiet && (processed <= 5 || processed % 100 == 0)) {
                std::cout << "Event " << md.sequence_number
                          << " bid=" << std::fixed << std::setprecision(4) << md.best_bid_price
                          << " ask=" << md.best_ask_price
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
