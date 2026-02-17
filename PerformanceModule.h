#ifndef PERFORMANCE_MODULE_H
#define PERFORMANCE_MODULE_H

#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <iomanip>

class PerformanceModule {
public:
    explicit PerformanceModule(size_t reserve_samples = 10000) {
        latency_samples_ns_.reserve(reserve_samples);
    }

    // Legacy interface — no-op for stdout, just counts events
    void track_event() {
        ++total_events_;
    }

    // Record a latency sample (nanoseconds)
    void record_latency(int64_t ns) {
        latency_samples_ns_.push_back(ns);
        ++total_events_;
    }

    // Set total wall time for throughput calculation
    void set_wall_time(std::chrono::steady_clock::duration wall_time) {
        wall_time_ = wall_time;
    }

    // Throughput: events/sec
    double throughput() const {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall_time_).count();
        if (ns == 0) return 0.0;
        return static_cast<double>(total_events_) / (static_cast<double>(ns) / 1e9);
    }

    // Sort samples and report latency percentiles
    void report_latency_percentiles() const {
        if (latency_samples_ns_.empty()) {
            std::cout << "No latency samples recorded.\n";
            return;
        }

        // Sort a copy for percentile computation
        std::vector<int64_t> sorted = latency_samples_ns_;
        std::sort(sorted.begin(), sorted.end());

        size_t n = sorted.size();
        auto pct = [&](double p) -> int64_t {
            size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
            return sorted[idx];
        };

        int64_t min_ns = sorted.front();
        int64_t max_ns = sorted.back();
        int64_t p50  = pct(0.50);
        int64_t p90  = pct(0.90);
        int64_t p99  = pct(0.99);
        int64_t p999 = pct(0.999);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "=== LATENCY PERCENTILES ===\n";
        std::cout << "  Samples: " << n << "\n";
        std::cout << "  Min:     " << min_ns << " ns (" << min_ns / 1000.0 << " us)\n";
        std::cout << "  p50:     " << p50  << " ns (" << p50  / 1000.0 << " us)\n";
        std::cout << "  p90:     " << p90  << " ns (" << p90  / 1000.0 << " us)\n";
        std::cout << "  p99:     " << p99  << " ns (" << p99  / 1000.0 << " us)\n";
        std::cout << "  p99.9:   " << p999 << " ns (" << p999 / 1000.0 << " us)\n";
        std::cout << "  Max:     " << max_ns << " ns (" << max_ns / 1000.0 << " us)\n";
        std::cout << "  Throughput: " << throughput() << " events/sec\n";
        std::cout << "===========================\n";
    }

    int total_events() const { return total_events_; }
    const std::vector<int64_t>& latency_samples() const { return latency_samples_ns_; }

private:
    std::vector<int64_t> latency_samples_ns_;
    int total_events_ = 0;
    std::chrono::steady_clock::duration wall_time_{};
};

#endif // PERFORMANCE_MODULE_H
