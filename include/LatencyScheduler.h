#ifndef LATENCY_SCHEDULER_H
#define LATENCY_SCHEDULER_H

// Discrete-event latency scheduler for the simulator side. Drives the three
// per-stage delays (feed, ack, matching) introduced in milestone M8:
//
//   feed     — sim-produced market data → delivered to MM
//   ack      — MM order submit         → live in matching engine
//   matching — counterparty aggressor  → fill reported back via market data
//
// Header-only, monomorphic, zero-alloc once `reserve()` is paid in the ctor.
// Pop order is strictly `(time_ns ASC, seq ASC)`: two events scheduled for
// the same nanosecond dequeue in *submission* order. That tiebreaker is what
// makes determinism survive — without it, ties resolve arbitrarily on heap
// rebalance.
//
// Payload is a small POD `LatencyEventKind + handle`. The simulator owns
// side-tables keyed on `handle` for the larger payloads (`MarketDataEvent`,
// pending order info, fill info). Keeping the scheduler payload tiny keeps
// the heap nodes 32 B — one cache line holds two — and avoids paying for
// moves of fat structs every percolation step.
//
// The scheduler clock (`now_ns()`) advances on `pop_next()` to the time of
// the popped event. `schedule()` requires `time_ns >= now_ns()`; scheduling
// in the past is a contract violation (asserted in debug).
//
// Sampler types live in the same header so the per-stage configuration is a
// single include for the simulator. Each sampler precomputes whatever the
// distribution needs at construction so `sample()` is one RNG draw + a few
// FLOPs — no work repeated per event.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "LatencyConfig.h"

namespace mme {

// ---- Scheduled event -------------------------------------------------------

enum class LatencyEventKind : std::uint8_t {
    FeedDeliver,      // sim-produced MD event reaches the MM
    OrderLand,        // MM-submitted order becomes live in matching engine
    FillReport,       // fill becomes visible in next MD event delivered to MM
};

struct LatencyEvent {
    std::int64_t     time_ns;
    std::uint64_t    seq;
    LatencyEventKind kind;
    std::uint64_t    handle;   // simulator-side side-table index
};

class LatencyScheduler {
public:
    explicit LatencyScheduler(std::size_t reserve_n = 1024) {
        heap_.reserve(reserve_n);
    }

    bool        empty() const noexcept { return heap_.empty(); }
    std::size_t size()  const noexcept { return heap_.size();  }

    std::int64_t now_ns() const noexcept { return now_ns_; }

    void schedule(std::int64_t time_ns, LatencyEventKind kind,
                  std::uint64_t handle) {
        assert(time_ns >= now_ns_ && "schedule in past");
        heap_.push_back(Node{time_ns, next_seq_++, kind, handle});
        std::push_heap(heap_.begin(), heap_.end(), Greater{});
    }

    // Precondition: !empty(). Advances now_ns() to the popped event's time.
    LatencyEvent pop_next() {
        assert(!heap_.empty());
        std::pop_heap(heap_.begin(), heap_.end(), Greater{});
        Node n = heap_.back();
        heap_.pop_back();
        now_ns_ = n.time_ns;
        return LatencyEvent{n.time_ns, n.seq, n.kind, n.handle};
    }

    // Precondition: !empty().
    std::int64_t peek_next_time() const noexcept {
        assert(!heap_.empty());
        return heap_.front().time_ns;
    }

    // Discard all events. Resets clock and sequence counter so the scheduler
    // is reusable across simulator runs without reallocation.
    void clear() noexcept {
        heap_.clear();
        now_ns_   = 0;
        next_seq_ = 0;
    }

private:
    struct Node {
        std::int64_t     time_ns;
        std::uint64_t    seq;
        LatencyEventKind kind;
        std::uint64_t    handle;
    };

    // Min-heap via std::*_heap (which builds a max-heap by default), so the
    // comparator returns true when `a` should sort *after* `b`.
    struct Greater {
        bool operator()(const Node& a, const Node& b) const noexcept {
            if (a.time_ns != b.time_ns) return a.time_ns > b.time_ns;
            return a.seq > b.seq;
        }
    };

    std::vector<Node> heap_;
    std::uint64_t     next_seq_ = 0;
    std::int64_t      now_ns_   = 0;
};

// ---- Per-stage latency sampler --------------------------------------------
// (LatencyDistribution + StageLatencyConfig live in LatencyConfig.h)

class StageSampler {
public:
    explicit StageSampler(const StageLatencyConfig& cfg)
        : kind_(cfg.kind), mean_ns_(cfg.mean_ns) {
        switch (kind_) {
            case LatencyDistribution::Zero:
            case LatencyDistribution::Constant:
                break;
            case LatencyDistribution::Exponential:
                assert(cfg.mean_ns > 0);
                rate_ = 1.0 / static_cast<double>(cfg.mean_ns);
                break;
            case LatencyDistribution::LogNormal: {
                // Convert (mean, stddev) in real space → (mu, sigma) in log
                // space:  log_var = ln(1 + v/m²); log_mu = ln(m) − log_var/2.
                assert(cfg.mean_ns > 0);
                assert(cfg.stddev_ns >= 0);
                const double m = static_cast<double>(cfg.mean_ns);
                const double v = static_cast<double>(cfg.stddev_ns)
                               * static_cast<double>(cfg.stddev_ns);
                const double log_var = std::log1p(v / (m * m));
                sigma_ = std::sqrt(log_var);
                mu_    = std::log(m) - 0.5 * log_var;
                break;
            }
        }
    }

    bool is_zero() const noexcept {
        return kind_ == LatencyDistribution::Zero;
    }

    std::int64_t sample(std::mt19937_64& rng) const {
        switch (kind_) {
            case LatencyDistribution::Zero:
                return 0;
            case LatencyDistribution::Constant:
                return mean_ns_;
            case LatencyDistribution::Exponential: {
                std::exponential_distribution<double> dist(rate_);
                const double x = dist(rng);
                return x <= 0.0 ? 0 : static_cast<std::int64_t>(x);
            }
            case LatencyDistribution::LogNormal: {
                std::lognormal_distribution<double> dist(mu_, sigma_);
                const double x = dist(rng);
                return x <= 0.0 ? 0 : static_cast<std::int64_t>(x);
            }
        }
        return 0;
    }

private:
    LatencyDistribution kind_;
    std::int64_t        mean_ns_;
    // Exponential.
    double              rate_ = 0.0;
    // LogNormal (log-space parameters).
    double              mu_   = 0.0;
    double              sigma_ = 0.0;
};

} // namespace mme

#endif // LATENCY_SCHEDULER_H
