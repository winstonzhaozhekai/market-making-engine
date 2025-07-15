#include <iostream>
#include <thread>
#include <chrono>
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "PerformanceModule.h"

using namespace std;
using namespace std::chrono;

int main() {
    // Initialize components
    MarketSimulator simulator("XYZ", 100.0, 0.1, 0.5, 10);
    MarketMaker mm;
    PerformanceModule perf;

    // Main simulation loop
    for (int i = 0; i < 1000; ++i) {
        auto md = simulator.generate_event(); // Generate market data
        perf.track_event(md);                 // Track performance
        mm.on_market_data(md);                // Process market data
        mm.report();                          // Report inventory and PnL

        // Optional: Log trades from MarketSimulator
        for (const auto& trade : md.trades) {
            std::cout << "Trade: " << trade.aggressor_side 
                      << " | Price: " << trade.price 
                      << " | Size: " << trade.size << std::endl;
        }

        this_thread::sleep_for(milliseconds(10)); // Simulate time delay
    }

    // Report overall performance metrics
    perf.report_performance();

    return 0;
}