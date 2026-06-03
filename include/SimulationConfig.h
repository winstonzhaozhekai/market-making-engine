#ifndef SIMULATION_CONFIG_H
#define SIMULATION_CONFIG_H

#include <cstdint>
#include <string>

#include "LatencyConfig.h"
#include "QueueReactiveLob.h"

enum class SimulationMode {
    Simulate,
    Replay
};

// M9: which order-book simulation model is active.
//   Legacy        — the pre-M9 "5 levels glued to mid" decoration generator
//                   with random walks on per-level qty + a 20%-per-step IOC
//                   aggressor. Source-of-truth: MarketSimulator::bid_levels_
//                   / ask_levels_. M5/M6/M7/M8 binary-log byte-equality
//                   contracts assume Legacy.
//   QueueReactive — Huang/Lehalle/Rosenbaum queue-reactive LOB
//                   (`include/QueueReactiveLob.h`). All synthetic orders
//                   rest in the matching engine; aggressor flow comes from
//                   HLR market-order events; MD events derive their level
//                   arrays from engine state (M9/3).
enum class LobModel : std::uint8_t {
    Legacy,
    QueueReactive,
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

    // M9: synthetic-LOB model selection. `Legacy` keeps existing tests
    // byte-equal (binary-log + determinism replay); `QueueReactive`
    // engages the HLR primitive in `include/QueueReactiveLob.h`. The
    // `hlr` block is consumed only when `lob_model == QueueReactive`,
    // so leaving it default-constructed is fine for Legacy runs.
    // `lob_seed` is an independent determinism axis from `seed`
    // (mid-drift) and `latency_seed`, so backtest sweeps can hold the
    // LOB path fixed while varying the latency model and vice versa.
    LobModel       lob_model = LobModel::Legacy;
    mme::HLRConfig hlr{};
    std::uint32_t  lob_seed = 0xB00B5u;

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
