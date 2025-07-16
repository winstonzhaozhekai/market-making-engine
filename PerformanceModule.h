#ifndef PERFORMANCE_MODULE_H
#define PERFORMANCE_MODULE_H

#include "MarketDataEvent.h"
#include <vector>
#include <chrono>

class PerformanceModule {
public:
    void track_event(const MarketDataEvent& md);
    void report_performance() const;
    long get_total_runtime() const; // New method
    double get_average_iteration_time() const; // New method

private:
    std::vector<std::chrono::system_clock::time_point> event_timestamps;
    int total_events = 0;
};

#endif // PERFORMANCE_MODULE_H