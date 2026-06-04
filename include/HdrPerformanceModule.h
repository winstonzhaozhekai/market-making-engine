#ifndef MME_HDR_PERFORMANCE_MODULE_H
#define MME_HDR_PERFORMANCE_MODULE_H

// HDR-backed drop-in for PerformanceModule's bench-facing surface.
//
// Additive by design (ROADMAP M11/1): the legacy PerformanceModule is
// still used by the WebSocket runtime (WsSession.cpp), whose outbound
// schema the frontend consumes. The bench binaries switch to this module
// so the published p99 / p99.9 numbers are HDR-interpolated rather than
// nearest-rank, while the WS path is left byte-for-byte unchanged.
//
// Interface mirrors PerformanceModule (record_latency / set_wall_time /
// throughput / report_latency_percentiles / total_events) so a bench can
// swap the type with no other edits.

#include "HdrHistogram.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <ostream>

namespace mme {

class HdrPerformanceModule {
public:
    // Default range covers 1 ns .. 1 s at 3 significant figures — the band
    // every per-event hot-path measurement in this engine falls in.
    explicit HdrPerformanceModule(std::int64_t lowest_ns = 1,
                                  std::int64_t highest_ns = 1'000'000'000,
                                  int significant_figures = 3)
        : hist_(lowest_ns, highest_ns, significant_figures) {}

    void track_event() { ++total_events_; }

    void record_latency(std::int64_t ns) {
        hist_.record_value(ns);
        ++total_events_;
    }

    void set_wall_time(std::chrono::steady_clock::duration wall_time) {
        wall_time_ = wall_time;
    }

    double throughput() const {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall_time_).count();
        if (ns == 0) return 0.0;
        return static_cast<double>(total_events_) / (static_cast<double>(ns) / 1e9);
    }

    const HdrHistogram& histogram() const { return hist_; }
    std::int64_t total_events() const { return total_events_; }

    void report_latency_percentiles(std::ostream& os = std::cout) const {
        if (hist_.total_count() == 0) {
            os << "No latency samples recorded.\n";
            return;
        }
        const std::int64_t min_ns = hist_.min();
        const std::int64_t max_ns = hist_.max();
        const std::int64_t p50 = hist_.value_at_quantile(0.50);
        const std::int64_t p90 = hist_.value_at_quantile(0.90);
        const std::int64_t p99 = hist_.value_at_quantile(0.99);
        const std::int64_t p999 = hist_.value_at_quantile(0.999);

        os << std::fixed << std::setprecision(2);
        os << "=== LATENCY PERCENTILES (HDR) ===\n";
        os << "  Samples: " << hist_.total_count() << "\n";
        os << "  Min:     " << min_ns << " ns (" << min_ns / 1000.0 << " us)\n";
        os << "  Mean:    " << hist_.mean() << " ns (" << hist_.mean() / 1000.0 << " us)\n";
        os << "  p50:     " << p50 << " ns (" << p50 / 1000.0 << " us)\n";
        os << "  p90:     " << p90 << " ns (" << p90 / 1000.0 << " us)\n";
        os << "  p99:     " << p99 << " ns (" << p99 / 1000.0 << " us)\n";
        os << "  p99.9:   " << p999 << " ns (" << p999 / 1000.0 << " us)\n";
        os << "  Max:     " << max_ns << " ns (" << max_ns / 1000.0 << " us)\n";
        os << "  Throughput: " << throughput() << " events/sec\n";
        os << "=================================\n";
    }

private:
    HdrHistogram hist_;
    std::int64_t total_events_ = 0;
    std::chrono::steady_clock::duration wall_time_{};
};

}  // namespace mme

#endif  // MME_HDR_PERFORMANCE_MODULE_H
