#!/usr/bin/env bash
#
# M11/5 — one-shot M11 benchmark suite. Builds the release binaries, runs
# every bench under perf, renders the engine flamegraph, and (if a tape is
# present) runs the real-tape backtest. All artifacts land in bench_results/.
#
# Usage:
#   scripts/run_m11_suite.sh [--iters N] [--events N] [--cpu N] [--rt]
#                            [--tape PATH] [--symbol SYM] [--out DIR]
#
# Intended for the Linux metal box (M11/6). It also runs on a dev box: the
# perf wrappers degrade to `available,0` and skip the SVG, so the CSV
# scaffolding can be exercised anywhere. Real counters + flamegraph require
# Linux perf_events.
#
# Recommended metal invocation (after `sudo linux_bench_env.sh apply`):
#   scripts/run_m11_suite.sh --cpu 2 --rt \
#       --tape data/itch/01302019.NASDAQ_ITCH50 --symbol AAPL

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_DIR"

ITERS=1000000
EVENTS=5000000
CPU=""
RT=0
TAPE=""
SYMBOL="AAPL"
OUT="bench_results"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --iters)  ITERS="$2"; shift 2 ;;
        --events) EVENTS="$2"; shift 2 ;;
        --cpu)    CPU="$2"; shift 2 ;;
        --rt)     RT=1; shift ;;
        --tape)   TAPE="$2"; shift 2 ;;
        --symbol) SYMBOL="$2"; shift 2 ;;
        --out)    OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
export BENCH_RT="$RT"
[[ -n "$CPU" ]] && export BENCH_CPU="$CPU"

PERF="$SCRIPT_DIR/bench_with_perf.sh"
FLAME="$SCRIPT_DIR/bench_flamegraph.sh"

# ---- Provenance header ------------------------------------------------------
META="$OUT/run_metadata.txt"
{
    echo "# M11 bench suite run"
    echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git_commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "git_dirty=$(test -n "$(git status --porcelain 2>/dev/null)" && echo yes || echo no)"
    echo "uname=$(uname -srm)"
    if [[ "$(uname -s)" == "Linux" ]]; then
        echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
        echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
    else
        echo "cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo '?')"
    fi
    echo "compiler=$(${CXX:-c++} --version | head -1)"
    echo "iters=$ITERS events=$EVENTS cpu=${CPU:-none} rt=$RT"
} >"$META"
echo "wrote $META" >&2

# ---- Build ------------------------------------------------------------------
echo "== building release ==" >&2
cmake --preset release >/dev/null
cmake --build --preset release \
    --target bench_engine bench_matching bench_accounting bench_risk bench_strategy itch_replay \
    >/dev/null
BIN=build/release

# ---- Per-component microbenches (each: own sections + perf_counters) --------
run_perf() { # run_perf <out.csv> <binary> [args...]
    local out="$1"; shift
    echo "== $1 -> $out ==" >&2
    # Single run: the binary truncates $out and writes its own section(s) via
    # --report-path; bench_with_perf.sh then appends the perf_counters block.
    "$PERF" --perf-csv "$out" -- "$@" --report-path "$out"
}

run_perf "$OUT/bench_matching.csv"   "$BIN/bench_matching"   --iters "$ITERS"
run_perf "$OUT/bench_accounting.csv" "$BIN/bench_accounting" --iters "$ITERS"
run_perf "$OUT/bench_risk.csv"       "$BIN/bench_risk"       --iters "$ITERS"
run_perf "$OUT/bench_strategy.csv"   "$BIN/bench_strategy"   --iters "$ITERS"

# ---- Whole-engine bench (heuristic + AS) under perf -------------------------
for strat in heuristic avellaneda-stoikov; do
    echo "== bench_engine ($strat) ==" >&2
    "$PERF" --perf-csv "$OUT/bench_engine_${strat}.perf.csv" -- \
        "$BIN/bench_engine" --events "$EVENTS" --seed 42 --strategy "$strat" \
        >"$OUT/bench_engine_${strat}.out.txt" 2>&1 || true
done

# ---- Flamegraph (engine hot path) -------------------------------------------
"$FLAME" --out "$OUT/bench_engine.svg" -- \
    "$BIN/bench_engine" --events "$EVENTS" --seed 42 --strategy heuristic || true

# ---- Real-tape backtest (if a tape is present) ------------------------------
if [[ -n "$TAPE" && -f "$TAPE" ]]; then
    echo "== itch_replay backtest: $SYMBOL on $TAPE ==" >&2
    "$BIN/itch_replay" --tape "$TAPE" --symbol "$SYMBOL" \
        --report-path "$OUT/itch_replay_${SYMBOL}.csv" || true
else
    echo "note: no --tape given or file missing; skipping real-tape backtest." >&2
    echo "      fetch one with scripts/download_sample_tape.sh first." >&2
fi

echo "== suite complete; artifacts in $OUT/ ==" >&2
ls -la "$OUT"/ >&2
