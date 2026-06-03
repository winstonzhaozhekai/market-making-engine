#ifndef SIMULATION_CONFIG_H
#define SIMULATION_CONFIG_H

#include <cstdint>
#include <string>

#include "LatencyConfig.h"

enum class SimulationMode {
    Simulate,
    Replay
};

struct SimulationConfig {
    std::string instrument = "XYZ";
    double tick_size = 0.01;
    double initial_price = 100.0;
    double spread = 0.1;
    double volatility = 0.5;

    // Per-stage latency (M8). All three default to Zero — the byte-equality
    // fast path that reproduces the pre-M8 deterministic-replay behavior.
    // `latency_seed` is independent from `seed` so a backtest can sweep
    // latency while holding the underlying market path fixed (and vice
    // versa) — essential for the fill-rate-vs-latency monotonicity check.
    mme::StageLatencyConfig feed_latency{};
    mme::StageLatencyConfig ack_latency{};
    mme::StageLatencyConfig matching_latency{};
    std::uint32_t latency_seed = 0xC0FFEEu;

    int iterations = 1000;
    std::uint32_t seed = 42;
    std::string event_log_path;
    std::string replay_log_path;
    SimulationMode mode = SimulationMode::Simulate;
    bool quiet = false;

    bool all_latencies_zero() const noexcept {
        return feed_latency.is_zero()
            && ack_latency.is_zero()
            && matching_latency.is_zero();
    }
};

#endif // SIMULATION_CONFIG_H
