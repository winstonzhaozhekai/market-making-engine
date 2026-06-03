#ifndef MME_QUEUE_REACTIVE_LOB_H
#define MME_QUEUE_REACTIVE_LOB_H

// Synthetic queue-reactive LOB primitive (Huang/Lehalle/Rosenbaum 2015,
// "Simulating and Analyzing Order Book Data: The Queue-Reactive Model").
//
// One `HLRSide` per book side. State is K integer queue sizes
// q[0..num_levels-1] at fixed price offsets from a reference price the
// wiring layer (MarketSimulator) chooses. Per-step dynamics on each side
// are three independent Bernoulli draws per level:
//
//   - limit-order arrival  at level i:   Bernoulli(lambda_per_ns[i] * dt)
//   - cancellation        at level i:    Bernoulli(mu_per_unit_per_ns
//                                                  * q_i * dt)
//   - market-order arrival at level 0:   Bernoulli(theta_per_ns * dt)
//
// Each event carries a `qty` of `mean_limit_size` / `mean_market_size`
// units (cancels and market orders cap at q to avoid negative queues).
// The Bernoulli approximation is exact for rate * dt → 0; the asserts
// keep the per-step probability inside the at-most-one-event-per-type
// regime where the approximation is accurate to second order.
//
// The primitive is intentionally engine-agnostic: it knows nothing about
// Ticks, reference prices, order ids, or the matching engine. The wiring
// layer translates emitted `HLREvent`s into add_order / cancel_order
// calls (and IOC market-order submissions) using whichever mapping it
// likes. This split makes the primitive trivially unit-testable.
//
// Determinism: caller-owned `std::mt19937_64`. Same seed + same config +
// same call sequence -> byte-identical event stream. This is why the
// RNG is passed by reference to `step()` rather than owned: the wiring
// layer holds the engine of determinism (`lob_seed`), the primitive is
// stateless w.r.t. randomness.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <vector>

#include "../Order.h"

namespace mme {

struct HLRConfig {
    static constexpr int kMaxLevels = 5;

    // Number of price levels per side. Levels 0..num_levels-1 are active.
    int num_levels = 5;

    // Limit-order arrival rate per nanosecond at each level (per side).
    // Default: ~1000 events/s at the inside with power-law decay across
    // depth, picked to give a non-trivial queue under the default mu.
    std::array<double, kMaxLevels> lambda_per_ns{{
        1.0e-6, 5.0e-7, 3.3e-7, 2.5e-7, 2.0e-7
    }};

    // Cancellation rate per unit-of-resting-qty per nanosecond. Total
    // cancel rate at level i is mu_per_unit_per_ns * q_i. Uniform across
    // levels for v1; HLR canonical extends to per-level mu_i.
    double mu_per_unit_per_ns = 1.0e-7;

    // Market-order arrival rate per ns at the inside (per side). Each
    // event consumes up to `mean_market_size` units from level 0.
    double theta_per_ns = 1.0e-8;

    // Mean number of units carried by limit-add / cancel / market events.
    int mean_limit_size  = 5;
    int mean_market_size = 5;

    // Initial queue depths the simulator should seed into the matching
    // engine for this side. Stored here so the primitive's q_[] and the
    // engine's actual depth start in sync.
    std::array<int, kMaxLevels> initial_queue{{
        20, 30, 40, 40, 40
    }};
};

enum class HLREventKind : std::uint8_t {
    LimitAdd,     // add `qty` units of resting liquidity at `level`
    Cancel,       // remove `qty` units of resting liquidity at `level`
    MarketOrder,  // aggressive order consumes `qty` units from level 0
};

struct HLREvent {
    HLREventKind kind;
    Side         side;
    int          level;
    int          qty;
};

class HLRSide {
public:
    HLRSide(Side side, const HLRConfig& cfg)
        : side_(side), cfg_(cfg), q_{} {
        assert(cfg_.num_levels > 0 &&
               cfg_.num_levels <= HLRConfig::kMaxLevels);
        for (int i = 0; i < cfg_.num_levels; ++i) {
            q_[i] = cfg_.initial_queue[i];
            assert(q_[i] >= 0);
        }
    }

    // Step forward by dt_ns. Appends sampled events to `out` (does not
    // clear). Returns the number of events appended this call.
    //
    // Asserts that each per-event-type probability stays below 0.5 so the
    // Bernoulli draw remains a valid approximation of Poisson(rate*dt).
    // Cancels can hit 0.5 transiently when q is enormous; the assert
    // catches misconfigured rates without preventing brief spikes.
    int step(std::int64_t dt_ns, std::mt19937_64& rng,
             std::vector<HLREvent>& out) {
        assert(dt_ns > 0);
        const double dt = static_cast<double>(dt_ns);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        int n_events = 0;

        for (int i = 0; i < cfg_.num_levels; ++i) {
            const double p_add = cfg_.lambda_per_ns[i] * dt;
            assert(p_add < 0.1);
            if (u(rng) < p_add) {
                out.push_back(HLREvent{
                    HLREventKind::LimitAdd, side_, i, cfg_.mean_limit_size
                });
                q_[i] += cfg_.mean_limit_size;
                ++n_events;
            }
        }

        for (int i = 0; i < cfg_.num_levels; ++i) {
            if (q_[i] <= 0) continue;
            const double p_cxl =
                cfg_.mu_per_unit_per_ns * static_cast<double>(q_[i]) * dt;
            assert(p_cxl < 0.5);
            if (u(rng) < p_cxl) {
                const int sz = std::min(q_[i], cfg_.mean_limit_size);
                out.push_back(HLREvent{
                    HLREventKind::Cancel, side_, i, sz
                });
                q_[i] -= sz;
                ++n_events;
            }
        }

        if (q_[0] > 0) {
            const double p_mkt = cfg_.theta_per_ns * dt;
            assert(p_mkt < 0.1);
            if (u(rng) < p_mkt) {
                const int sz = std::min(q_[0], cfg_.mean_market_size);
                out.push_back(HLREvent{
                    HLREventKind::MarketOrder, side_, 0, sz
                });
                q_[0] -= sz;
                ++n_events;
            }
        }

        return n_events;
    }

    int          queue_at(int level) const { return q_[level]; }
    int          num_levels()        const { return cfg_.num_levels; }
    Side         side()              const { return side_; }
    const HLRConfig& config()        const { return cfg_; }

    // Externally-applied decrement. Used when an event outside the HLR
    // process (e.g., a MM fill consuming a synthetic order at the same
    // price) reduces resting qty at a level. Unused in v1 because MM
    // does not share queues with HLR.
    void on_external_decrement(int level, int qty) {
        assert(level >= 0 && level < cfg_.num_levels);
        q_[level] = std::max(0, q_[level] - qty);
    }

private:
    Side                                   side_;
    HLRConfig                              cfg_;
    std::array<int, HLRConfig::kMaxLevels> q_;
};

} // namespace mme

#endif  // MME_QUEUE_REACTIVE_LOB_H
