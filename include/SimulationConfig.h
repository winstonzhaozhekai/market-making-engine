#ifndef SIMULATION_CONFIG_H
#define SIMULATION_CONFIG_H

#include <cstdint>
#include <string>

#include "LatencyConfig.h"
#include "QueueReactiveLob.h"

enum class SimulationMode {
    Simulate,
    Replay,
    // M10: drive the matching engine off a Nasdaq TotalView-ITCH 5.0 binary
    // tape. The tape is the source of synthetic resting orders; MM injects
    // via the same `MarketSimulator::submit_order` path the other modes use,
    // and the matching engine's price-time-priority FIFO arbitrates queue
    // position between MM and ITCH-resting orders. Requires
    // `itch_log_path` + `itch_symbol`; latency is required to be zero for
    // v1 (the M8 fixed 1 µs production-step does not align with ITCH's
    // non-uniform timestamp deltas; latency overlay is deferred).
    ItchReplay
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

    // M9: synthetic-LOB model selection. `QueueReactive` is the
    // canonical post-M9 mode — HLR-driven synthetic orders rest in the
    // matching engine, aggressor flow comes from theta-driven market-
    // order events, and MD level arrays are derived from engine state
    // (`include/QueueReactiveLob.h`). `Legacy` keeps the pre-M9
    // "5 levels glued to mid" decoration generator + 20%-per-step IOC
    // aggressor for tests written against that path (M5/M6/M7/M8
    // regression tests pin to it explicitly so their byte-equality and
    // monotone-decline contracts hold). `lob_seed` is an independent
    // determinism axis from `seed` (Legacy brownian) and `latency_seed`,
    // so backtest sweeps can hold the LOB path fixed while varying the
    // latency model and vice versa.
    LobModel       lob_model = LobModel::QueueReactive;
    mme::HLRConfig hlr{};
    std::uint32_t  lob_seed = 0xB00B5u;

    int iterations = 1000;
    std::uint32_t seed = 42;
    std::string event_log_path;
    std::string replay_log_path;

    // M10 ITCH replay: path to a Nasdaq TotalView-ITCH 5.0 binary tape
    // (length-prefixed message stream) plus the 8-char ASCII symbol to
    // filter on (e.g. "AAPL    "). Ignored unless `mode ==
    // SimulationMode::ItchReplay`.
    std::string    itch_log_path;
    std::string    itch_symbol;

    SimulationMode mode = SimulationMode::Simulate;
    bool quiet = false;

    bool all_latencies_zero() const noexcept {
        return feed_latency.is_zero()
            && ack_latency.is_zero()
            && matching_latency.is_zero();
    }
};

#endif // SIMULATION_CONFIG_H
