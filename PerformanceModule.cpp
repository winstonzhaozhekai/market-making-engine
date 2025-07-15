#include "PerformanceModule.h"
#include <iostream>

void PerformanceModule::track_event(const MarketDataEvent& md) {
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