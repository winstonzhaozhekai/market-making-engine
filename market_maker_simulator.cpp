#include <iostream>
#include <thread>
#include <chrono>
#include <signal.h>
#include <iomanip> // Add this for setprecision
#include "MarketSimulator.h"
#include "MarketMaker.h"
#include "PerformanceModule.h"

using namespace std;
using namespace std::chrono;

volatile bool running = true;

void signal_handler(int signal) {
    running = false;
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully...\n";
}

int main() {
    // Set up signal handling for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize components
    MarketSimulator simulator("XYZ", 100.0, 0.1, 0.5, 10);
    MarketMaker mm;
    PerformanceModule perf;
    
    std::cout << "Starting Market Making Engine..." << std::endl;
    std::cout << "Press Ctrl+C to stop gracefully" << std::endl;
    
    int iteration = 0;
    auto start_time = steady_clock::now();
    
    // Main simulation loop
    while (running && iteration < 1000) {
        try {
            auto md = simulator.generate_event();
            
            // Track performance
            perf.track_event(md);
            
            // Process market data
            mm.on_market_data(md);
            
            // Report every 100 iterations
            if (iteration % 100 == 0) {
                mm.report();
                std::cout << "Iteration: " << iteration << std::endl;
            }
            
            // Log trades with more detail
            for (const auto& trade : md.trades) {
                std::cout << "Trade: " << trade.aggressor_side 
                          << " | Price: " << fixed << setprecision(4) << trade.price
                          << " | Size: " << trade.size 
                          << " | ID: " << trade.trade_id << std::endl;
            }
            
            // Log partial fills
            for (const auto& fill : md.partial_fills) {
                std::cout << "Partial Fill: " << fill.order_id
                          << " | Filled: " << fill.filled_size
                          << " | Remaining: " << fill.remaining_size
                          << " | Price: " << fixed << setprecision(4) << fill.price << std::endl;
            }
            
            iteration++;
            this_thread::sleep_for(milliseconds(10));
            
        } catch (const std::exception& e) {
            std::cerr << "Error in simulation loop: " << e.what() << std::endl;
            break;
        }
    }
    
    auto end_time = steady_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    
    std::cout << "\n=== SIMULATION COMPLETE ===" << std::endl;
    std::cout << "Total iterations: " << iteration << std::endl;
    std::cout << "Total runtime: " << duration.count() << "ms" << std::endl;
    std::cout << "Average iteration time: " << (duration.count() / static_cast<double>(iteration)) << "ms" << std::endl;
    
    // Final reports
    mm.report();
    perf.report_performance();
    
    return 0;
}