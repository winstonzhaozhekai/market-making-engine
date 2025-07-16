#include "PerformanceModule.h"
#include <iostream>
#include <chrono>

void PerformanceModule::track_event(const MarketDataEvent& md) {
    auto now = std::chrono::system_clock::now();
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - md.timestamp).count();
    std::cout << "Event latency: " << latency << " ms" << std::endl;

    event_timestamps.push_back(md.timestamp);
    ++total_events;
}

void PerformanceModule::report_performance() const {
    std::cout << "Total Events Processed: " << total_events << std::endl;
    if (!event_timestamps.empty()) {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            event_timestamps.back() - event_timestamps.front());
        std::cout << "Simulation Duration: " << duration.count() << " ms" << std::endl;
    }
}

long PerformanceModule::get_total_runtime() const {
    if (event_timestamps.empty()) return 0;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        event_timestamps.back() - event_timestamps.front());
    return duration.count();
}

double PerformanceModule::get_average_iteration_time() const {
    if (total_events == 0) return 0.0;
    return static_cast<double>(get_total_runtime()) / total_events;
}