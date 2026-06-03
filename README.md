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
cmake --preset release && cmake --build --preset release
./build/release/bench_engine --events 1000000 --seed 42 --strategy heuristic
./build/release/bench_engine --events 1000000 --seed 42 --strategy avellaneda-stoikov
```

## What Is Implemented

- Deterministic simulation config and seeded runs (`--seed`, `--iterations`, per-stage `--feed-latency-*` / `--ack-latency-*` / `--matching-latency-*`, `--latency-seed`)
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
- C++20 compiler (Apple clang 16+, GCC 11+, or Clang 15+)
- CMake 3.21+ and Ninja
- vcpkg (manifest mode); `VCPKG_ROOT` exported in your shell. One-time setup:
  ```bash
  git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
  "$HOME/vcpkg/bootstrap-vcpkg.sh"
  export VCPKG_ROOT="$HOME/vcpkg"   # add to your shell rc
  ```
  Boost (`asio`, `beast`, `system`, `thread`) and GoogleTest are declared in `vcpkg.json` and installed automatically on first configure.

### Frontend
- Node.js + npm

## Build And Run

```bash
cmake --preset release
cmake --build --preset release
```

Other configure presets: `default` (Debug), `asan` (Debug + ASan/UBSan), `ubsan`, `tsan`. Build outputs land in `build/<preset>/`.

### CLI simulator

```bash
./build/release/market_maker_simulator --help
```

Key options:
- `--mode simulate|replay`
- `--strategy heuristic|avellaneda-stoikov`
- `--seed <n>`
- `--iterations <n>`
- Per-stage latency (M8): `--feed-latency-dist {zero|constant|exponential|lognormal}`, `--feed-latency-mean-ns <int>`, `--feed-latency-stddev-ns <int>`, and the same triplet for `--ack-latency-*` and `--matching-latency-*`. All default to `zero` (byte-equal deterministic-replay path).
- `--latency-seed <uint>` — RNG seed for latency draws, independent from `--seed`
- `--event-log <path>`
- `--replay <path>`
- `--engine-log <path>`
- `--quiet`

Example deterministic run (all latencies zero — byte-equal replay path):

```bash
./build/release/market_maker_simulator --strategy heuristic --seed 42 --iterations 1000 --quiet
```

Event log + replay:

```bash
./build/release/market_maker_simulator --seed 7 --iterations 1000 --event-log /tmp/mm.log --quiet
./build/release/market_maker_simulator --mode replay --replay /tmp/mm.log --iterations 1000 --quiet
```

Sweep feed latency (lognormal, 50 µs mean, 25 µs stddev) to see fill-rate degrade:

```bash
./build/release/market_maker_simulator --strategy heuristic --seed 42 --iterations 100000 \
    --feed-latency-dist lognormal --feed-latency-mean-ns 50000 --feed-latency-stddev-ns 25000 \
    --latency-seed 0xC0FFEE --quiet
```

### WebSocket server + frontend

1. Start server (port `8080`):

```bash
./build/release/WebSocketServer
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
- `set_strategy:heuristic|avellaneda-stoikov`
- Per-stage latency (M8) — for each of `feed`, `ack`, `matching`:
  - `set_{stage}_latency_dist:zero|constant|exponential|lognormal`
  - `set_{stage}_latency_mean_ns:<int64>`
  - `set_{stage}_latency_stddev_ns:<int64>`
- `set_latency_seed:<uint32>`
- `set_max_net_position:<int>`
- `set_max_notional_exposure:<double>`
- `set_max_drawdown:<double>`

Outbound message types (all include `schema_version`):
- `status`
- `error`
- `simulation_update` with top-of-book/trades plus metrics (PnL, drawdown, exposure, fills, throughput, risk state, strategy)

## Tests

Run the full GoogleTest suite via ctest:

```bash
ctest --preset release
```

Or under sanitizers:

```bash
ctest --preset asan
ctest --preset ubsan
```

Test binaries (`build/<preset>/tests/`):
- `test_determinism`
- `test_matching_engine`
- `test_accounting`
- `test_risk_manager`
- `test_strategy_behavior`
- `test_ws_protocol`

## Benchmarking

```bash
cmake --build --preset release --target bench_engine
./build/release/bench_engine --events 1000000 --seed 42 --strategy heuristic
./build/release/bench_engine --events 1000000 --seed 42 --strategy avellaneda-stoikov
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

## Roadmap

Future work, planned iterations, known correctness gaps, and the architecture decisions are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md). This README intentionally describes only what is currently implemented.

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
