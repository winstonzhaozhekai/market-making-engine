# HFT Market Making Engine

## Overview

The **HFT Market Making Engine** is a high-frequency trading simulation platform designed to model market-making strategies, track performance metrics, and visualize trading activity. It includes the following components:

1. **MarketSimulator**: Generates realistic market data events for simulation.
2. **MarketMaker**: Implements market-making strategies and processes market data.
3. **PerformanceModule**: Tracks and reports performance metrics.
4. **WebSocketServer**: Streams simulation data to a frontend for real-time visualization.
5. **Frontend**: Provides an interactive dashboard to monitor simulation metrics and trading activity.

## Features

- **Market Simulation**: Generates realistic market data, including trades, order book updates, and partial fills.
- **Market-Making Strategies**: Implements inventory management, risk limits, and quoting logic.
- **Performance Tracking**: Tracks latency, profit and loss (PnL), slippage, and missed opportunities.
- **Real-Time Visualization**: Streams simulation data to a React-based frontend for live monitoring.
- **WebSocket Integration**: Enables real-time communication between the backend and frontend.

## Requirements

### Backend
- **C++ Compiler**: Supports C++17 or later.
- **Boost Libraries**: Required for WebSocket functionality.
- **Dependencies**: Includes `std::thread`, `std::chrono`, and Boost's `asio` and `beast`.

### Frontend
- **Node.js**: Required to run the React application.
- **Dependencies**: Includes `react`, `chart.js`, and `websocket`.

## Installation

### Backend Setup
1. Clone the repository:
   ```bash
   git clone https://github.com/winstonzhaozhekai/hft-market-making-engine.git
   cd hft-market-making-engine
   ```

2. Install Boost libraries (if not already installed):
   ```bash
   brew install boost
   ```

3. Build the backend:
   ```bash
   make
   ```

### Frontend Setup
1. Navigate to the `frontend` directory:
   ```bash
   cd frontend
   ```

2. Install dependencies:
   ```bash
   npm install
   ```

3. Start the frontend:
   ```bash
   npm start
   ```

## Usage

### Running the Backend
Start the WebSocket server:
```bash
./WebSocketServer
```

Run the market-making simulation:
```bash
./market_maker_simulator
```

### Running the Frontend
Open the frontend in your browser:
```bash
http://localhost:3000
```

### Simulation Workflow
1. Start the WebSocket server to stream data.
2. Launch the frontend to monitor simulation metrics.
3. Run the market-making simulation to generate and process market data.

## File Structure

### Backend
- **`market_maker_simulator.cpp`**: Main entry point for the simulation.
- **`MarketSimulator.h` / `MarketSimulator.cpp`**: Handles market data generation.
- **`MarketMaker.h` / `MarketMaker.cpp`**: Implements market-making logic.
- **`PerformanceModule.h` / `PerformanceModule.cpp`**: Tracks and reports performance metrics.
- **`WebSocketServer.cpp`**: Streams simulation data to the frontend.

### Frontend
- **`src/App.js`**: Main React component for the dashboard.
- **`src/components/Logger.jsx`**: Displays WebSocket messages.
- **`src/index.js`**: Entry point for the React application.
- **`public/index.html`**: HTML template for the frontend.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.