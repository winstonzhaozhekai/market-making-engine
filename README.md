# HFT Market Making Engine

## Overview

This project is a high-frequency trading (HFT) market-making engine designed to simulate market data, process it, and track performance metrics. It includes three main components:

1. **MarketSimulator**: Generates market data events for simulation.
2. **MarketMaker**: Processes market data and performs market-making operations.
3. **PerformanceModule**: Tracks and reports performance metrics.

## Features

- Simulates market data for a given stock.
- Implements market-making strategies.
- Tracks inventory, profit and loss (PnL), and other performance metrics.
- Provides detailed performance reports.

## Requirements

- **C++ Compiler**: Ensure you have a modern C++ compiler that supports C++17 or later.
- **Dependencies**: Include any required libraries or frameworks (e.g., `std::thread`, `std::chrono`).

## Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/winstonzhaozhekai/hft-market-making-engine.git
   cd hft-market-making-engine
   ```

2. Build the project:
   ```bash
   mkdir build
   cd build
   cmake ..
   make
   ```

## Usage

Run the simulation:
```bash
./market_maker_simulator
```

## File Structure

- **`market_maker_simulator.cpp`**: Main entry point for the simulation.
- **`MarketSimulator.h` / `MarketSimulator.cpp`**: Handles market data generation.
- **`MarketMaker.h` / `MarketMaker.cpp`**: Implements market-making logic.
- **`PerformanceModule.h` / `PerformanceModule.cpp`**: Tracks and reports performance metrics.

## Contributing

Contributions are welcome! Please fork the repository and submit a pull request.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

## Contact

For questions or feedback, please contact [your email or GitHub profile].