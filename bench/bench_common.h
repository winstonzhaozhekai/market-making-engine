#ifndef MME_BENCH_COMMON_H
#define MME_BENCH_COMMON_H

// Shared helpers for the M11 per-component microbenches (bench_matching,
// bench_accounting, bench_risk, bench_strategy). Each microbench isolates
// one hot-path operation, times it per-call with steady_clock into an HDR
// histogram, and emits a multi-section CSV using the same `# section: …`
// convention as bench_lob_realism / itch_replay so a consumer can split
// the output with awk/pandas.
//
// CSV shape per section:
//   # section: <name>
//   metric,value_ns
//   samples,<n>
//   min,<ns>
//   mean,<ns>
//   p50,<ns>
//   p90,<ns>
//   p99,<ns>
//   p99_9,<ns>
//   max,<ns>
//   throughput_per_sec,<ev/s>
//
// A human-readable copy of each section goes to stderr so a bare run is
// legible without post-processing.

#include "include/HdrPerformanceModule.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <string>

namespace mme::bench {

// Volatile sink to keep the optimizer from eliminating the timed work.
// Microbenches accumulate every result they produce into this.
inline volatile std::int64_t g_sink = 0;

inline void emit_section(std::ostream& csv, const std::string& name,
                         const HdrPerformanceModule& perf) {
    const auto& h = perf.histogram();
    csv << "# section: " << name << "\n";
    csv << "metric,value_ns\n";
    csv << "samples," << h.total_count() << "\n";
    csv << "min," << h.min() << "\n";
    csv << "mean," << h.mean() << "\n";
    csv << "p50," << h.value_at_quantile(0.50) << "\n";
    csv << "p90," << h.value_at_quantile(0.90) << "\n";
    csv << "p99," << h.value_at_quantile(0.99) << "\n";
    csv << "p99_9," << h.value_at_quantile(0.999) << "\n";
    csv << "max," << h.max() << "\n";
    csv << "throughput_per_sec," << perf.throughput() << "\n";
    csv << "\n";

    std::cerr << "[" << name << "] samples=" << h.total_count()
              << " min=" << h.min() << "ns"
              << " p50=" << h.value_at_quantile(0.50) << "ns"
              << " p99=" << h.value_at_quantile(0.99) << "ns"
              << " p99.9=" << h.value_at_quantile(0.999) << "ns"
              << " max=" << h.max() << "ns"
              << " thr=" << perf.throughput() << "/s\n";
}

// Steady-clock timestamp helper kept in one place so every microbench
// measures the same way as bench_engine.
inline std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

inline std::int64_t ns_between(std::chrono::steady_clock::time_point a,
                               std::chrono::steady_clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

}  // namespace mme::bench

#endif  // MME_BENCH_COMMON_H
