#ifndef SIMULATION_CONFIG_H
#define SIMULATION_CONFIG_H

#include <cstdint>
#include <string>

enum class SimulationMode {
    Simulate,
    Replay
};

struct SimulationConfig {
    std::string instrument = "XYZ";
    double initial_price = 100.0;
    double spread = 0.1;
    double volatility = 0.5;
    int latency_ms = 10;
    int iterations = 1000;
    uint32_t seed = 42;
    std::string event_log_path;
    std::string replay_log_path;
    SimulationMode mode = SimulationMode::Simulate;
    bool quiet = false;
};

#endif // SIMULATION_CONFIG_H
