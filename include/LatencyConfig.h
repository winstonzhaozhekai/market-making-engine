#ifndef LATENCY_CONFIG_H
#define LATENCY_CONFIG_H

// Per-stage latency configuration types. Split out from LatencyScheduler.h
// so the lightweight POD config can be embedded in SimulationConfig (and
// other transport-layer types) without dragging <random>/<cmath> through
// every consumer.

#include <cstdint>

namespace mme {

enum class LatencyDistribution : std::uint8_t {
    Zero,         // always 0 ns           — byte-equality fast-path sentinel
    Constant,     // always mean_ns
    Exponential,  // rate = 1/mean_ns      (memoryless inter-arrival shape)
    LogNormal,    // (mean_ns, stddev_ns)  in real space, mapped to log space
};

struct StageLatencyConfig {
    LatencyDistribution kind      = LatencyDistribution::Zero;
    std::int64_t        mean_ns   = 0;
    std::int64_t        stddev_ns = 0;   // ignored unless kind == LogNormal

    bool is_zero() const noexcept {
        return kind == LatencyDistribution::Zero;
    }
};

} // namespace mme

#endif // LATENCY_CONFIG_H
