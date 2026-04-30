# Market Making Engine

HFT-style market making simulator built in C++ with deterministic replay, matching/accounting/risk modules, a WebSocket runtime, and a React analytics UI.

## Performance

Order-handling hot path (`MarketMaker::on_market_data`), measured per-event with `std::chrono::steady_clock`, 1,000,000 events, seed 42, `-O3 -march=native -flto`:

| Strategy            | p90    | p99    | p99.9   | Throughput     | Wall   |
|---------------------|--------|--------|---------|----------------|--------|
| heuristic           | 42 ns  | 167 ns | 1.00 µs | 3.07 M ev/sec  | 326 ms |
| avellaneda-stoikov  | 42 ns  | 292 ns | 1.12 µs | 3.17 M ev/sec  | 315 ms |

p50 is omitted because the per-event work falls below the host clock's tick resolution (~41 ns on Apple Silicon `mach_absolute_time`); p90/p99/throughput are the meaningful figures.

Hardware: Apple M4 Pro (12-core), 24 GB, macOS 15.6, Apple clang 17. Reproduce:

```bash
make bench
./bench/bench_engine --events 1000000 --seed 42 --strategy heuristic
./bench/bench_engine --events 1000000 --seed 42 --strategy avellaneda-stoikov
```

## What Is Implemented

- Deterministic simulation config and seeded runs (`--seed`, `--iterations`, `--latency-ms`)
- Replay mode from event log (`--mode replay --replay <path>`)
- Matching engine with price-time priority, partial/full fills, cancel flow
- Order lifecycle states (`NEW`, `ACKNOWLEDGED`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED`)
- Accounting with realized/unrealized PnL, cost basis, avg entry, fees/rebates, gross/net exposure
- Risk engine with:
  - max net position
  - max notional exposure
  - max drawdown + high-water mark
  - max quote and cancel rates
  - stale market data guard
  - max spread guard
  - cooldown-based recovery and kill-switch state
- Strategy interface with:
  - `heuristic` strategy
  - `avellaneda-stoikov` strategy with rolling volatility + OFI estimators, inventory-aware reservation price, dynamic spread, optional toxic-flow pullback
- Performance tooling:
  - benchmark binary (`bench/bench_engine`)
  - latency percentiles (`p50`, `p90`, `p99`, `p99.9`)
  - optional compact binary event logging (`--binary-log`)
- WebSocket runtime robustness:
  - per-session outbound queue + serialized writes
  - session lifecycle cleanup
  - overlap guard for concurrent simulations (opt-in)
  - heartbeat ping + inactivity timeout
  - versioned outbound schema (`schema_version`)
- Frontend analysis UI:
  - run controls (seed, strategy, run length, latency, key risk limits)
  - single-run and queued A/B comparison mode
  - charts for inventory, spread, PnL, drawdown
  - capped/virtualized log view for long runs

## Architecture

- `MarketSimulator`: Generates LOB snapshots/trades, routes simulated aggressive flow into `MatchingEngine`, supports log write/replay.
- `MatchingEngine`: Stores MM resting orders and matches incoming flow with price-time priority.
- `MarketMaker`: Consumes market data, processes fills, marks to market, evaluates risk, and quotes via pluggable strategy.
- `Accounting`: Source of truth for position, cost basis, PnL, fees/rebates, exposures.
- `RiskManager`: Rule engine + state machine (`Normal`, `Warning`, `Breached`, `KillSwitch`).
- `WsSession`/`WebSocketServer`: Per-client simulation sessions and streaming updates.
- `frontend/`: React dashboard for run control and post-trade analytics.

## Build Requirements

### Backend
- C++17 compiler (`g++` used in `Makefile`)
- Boost (`asio`/`beast`) for WebSocket targets
- `make`

Note: `Makefile` currently uses Homebrew Boost paths:
- `BOOST_INCLUDE = -I/opt/homebrew/Cellar/boost/1.88.0/include`
- `BOOST_LIB = -L/opt/homebrew/Cellar/boost/1.88.0/lib`

Update those if your Boost install path differs.

### Frontend
- Node.js + npm

## Build And Run

```bash
make all
```

### CLI simulator

```bash
./market_maker_simulator --help
```

Key options:
- `--mode simulate|replay`
- `--strategy heuristic|avellaneda-stoikov`
- `--seed <n>`
- `--iterations <n>`
- `--latency-ms <n>`
- `--event-log <path>`
- `--replay <path>`
- `--binary-log <path>`
- `--quiet`

Example deterministic run:

```bash
./market_maker_simulator --strategy heuristic --seed 42 --iterations 1000 --latency-ms 0 --quiet
```

Event log + replay:

```bash
./market_maker_simulator --seed 7 --iterations 1000 --latency-ms 0 --event-log /tmp/mm.log --quiet
./market_maker_simulator --mode replay --replay /tmp/mm.log --iterations 1000 --latency-ms 0 --quiet
```

### WebSocket server + frontend

1. Start server (port `8080`):

```bash
./WebSocketServer
```

2. Start frontend:

```bash
cd frontend
npm install
npm start
```

3. Open `http://localhost:3000`

## WebSocket Protocol (Current)

Inbound commands:
- `run_simulation`
- `stop_simulation`
- `enable_overlap` / `disable_overlap`
- `set_seed:<uint32>`
- `set_iterations:<int>`
- `set_latency_ms:<int>`
- `set_strategy:heuristic|avellaneda-stoikov`
- `set_max_net_position:<int>`
- `set_max_notional_exposure:<double>`
- `set_max_drawdown:<double>`

Outbound message types (all include `schema_version`):
- `status`
- `error`
- `simulation_update` with top-of-book/trades plus metrics (PnL, drawdown, exposure, fills, throughput, risk state, strategy)

## Tests

Run full test suite:

```bash
make test
```

Included test binaries:
- `tests/test_determinism`
- `tests/test_matching_engine`
- `tests/test_accounting`
- `tests/test_risk_manager`
- `tests/test_strategy_behavior`
- `tests/test_ws_protocol`

## Benchmarking

```bash
make bench
./bench/bench_engine --events 1000000 --seed 42 --strategy heuristic
./bench/bench_engine --events 1000000 --seed 42 --strategy avellaneda-stoikov
```

Flags:
- `--events <n>`: number of market-data events to drive (default 10000)
- `--seed <n>`: simulator seed (default 42)
- `--strategy heuristic|avellaneda-stoikov`: strategy under test (default `heuristic`)

The harness measures `MarketMaker::on_market_data` per-event with `std::chrono::steady_clock`, suppresses MarketMaker stdout inside the timed region, and reports min/p50/p90/p99/p99.9/max plus wall-time throughput. See the **Performance** section above for representative numbers.

Profiling helper:

```bash
./scripts/profile.sh 100000 42
```

## Known Gaps / Next

- Architecture and experiment docs (`docs/ARCHITECTURE.md`, `docs/EXPERIMENTS.md`) are not present yet.
- CLI/front-end runtime config currently exposes only a subset of risk knobs.
- Replay log serialization includes market data/trades/partial fills; `mm_fills` are not reconstructed from replay log.
- The current simulator uses synthetic event generation and does not model full queue-position dynamics in an exchange-grade LOB.

Planned future iterations:
- **P0 foundation**
  - Upgrade matching internals to a production-style order book (price-level map + FIFO queues + O(1) cancel lookup).
  - Migrate hot-path price representation to integer fixed-point ticks.
  - Add experiment harness and quant metrics pipeline (Sharpe, drawdown, fill rate, inventory distribution, adverse selection, parameter sweeps).
- **P1 performance and simulation realism**
  - Add SPSC lock-free ring buffer for thread-to-thread event handoff, with cache-line-padded (`alignas(64)` / `hardware_destructive_interference_size`) producer and consumer indices to eliminate false sharing on the handoff.
  - Add CRTP strategy dispatch path for lower call overhead on benchmark/hot-path builds.
  - Move simulator toward incremental/event-driven LOB evolution (order arrivals, cancellations, market orders, queue position).
- **P2 engineering quality and model completion**
  - Add CMake build, sanitizer targets (ASan/UBSan/TSan), and CI workflows.
  - Add a CI-runnable Google Benchmark suite alongside the existing `std::chrono` harness so per-component microbenchmarks (matching, accounting, risk, strategy) can be tracked over time.
  - Add a Linux profiling target driven by `perf stat` / `perf record` for branch-miss, cache-miss, and IPC counters on the hot path, plus an Instruments equivalent for macOS dev runs.
  - Complete Avellaneda-Stoikov calibration features (decaying horizon and fill-data-driven kappa estimation).
- **P3 advanced differentiation**
  - Extend to multi-symbol simulation with correlation-aware, portfolio-level risk controls.
  - Integrate microstructure features (e.g., VPIN, Hawkes intensity, Kyle's lambda) into quoting decisions.
- **Performance goals to validate improvements**
  - Improve latency/throughput baseline after order book and dispatch upgrades.
  - Record before/after benchmark history across major iterations.

## Repository Map

- `market_maker_simulator.cpp`: CLI entrypoint
- `MarketSimulator.*`: event generation + replay
- `MatchingEngine.*`: order matching
- `MarketMaker.*`: quoting/fill handling/risk+accounting integration
- `include/Accounting.h`: accounting model
- `include/RiskManager.h` + `RiskManager.cpp`: risk engine
- `include/Strategy.h`, `include/HeuristicStrategy.h`, `strategies/AvellanedaStoikovStrategy.*`
- `WsSession.cpp`, `include/WsSession.h`, `WebSocketServer.cpp`: WS runtime
- `bench/bench_engine.cpp`: benchmark harness
- `tests/`: unit/integration tests
- `frontend/`: React analysis dashboard

## License

MIT (`LICENSE`)
