#!/usr/bin/env bash
#
# M11/4 — record a bench binary with `perf record` and render a flamegraph
# SVG via the vendored FlameGraph scripts (third_party/FlameGraph, pinned).
#
# Usage:
#   scripts/bench_flamegraph.sh [--out SVG] [--freq HZ] -- <binary> [args...]
#
# Example:
#   scripts/bench_flamegraph.sh --out bench_results/bench_engine.svg -- \
#       ./build/release/bench_engine --events 5000000 --seed 42
#
# Pipeline (the standard one):
#   perf record -F <freq> -g -- <binary> [args]
#   perf script | stackcollapse-perf.pl | flamegraph.pl > out.svg
#
# Defaults: -F 997 (a prime sampling rate, avoids lock-step aliasing with
# periodic workloads), call-graph via -g. Build the binary RelWithDebInfo
# (release preset) so frame symbols and inlined-frame info survive.
#
# Benchmarking hygiene (best-effort, probed): BENCH_CPU=<n> pins via
# taskset, BENCH_RT=1 runs SCHED_FIFO via chrt -r 99.
#
# Non-Linux / perf-absent hosts: warns and exits 0 (no-op) without
# producing an SVG, so the script is safe to invoke from any dev box. Real
# flamegraphs require Linux perf_events.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FG_DIR="$PROJECT_DIR/third_party/FlameGraph"

OUT="flamegraph.svg"
FREQ=997
ARGS=()
parsing_self=1
while [[ $# -gt 0 ]]; do
    if [[ $parsing_self -eq 1 ]]; then
        case "$1" in
            --out)  OUT="$2"; shift 2; continue ;;
            --freq) FREQ="$2"; shift 2; continue ;;
            --)     parsing_self=0; shift; continue ;;
            -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
            *) echo "Unknown wrapper arg: $1 (did you forget '--'?)" >&2; exit 2 ;;
        esac
    else
        ARGS+=("$1"); shift
    fi
done

if [[ ${#ARGS[@]} -eq 0 ]]; then
    echo "error: no binary given. Usage: $0 [--out SVG] [--freq HZ] -- <binary> [args...]" >&2
    exit 2
fi

# ---- Host capability gate ---------------------------------------------------
if [[ "$(uname -s)" != "Linux" ]] || ! command -v perf >/dev/null 2>&1; then
    echo "warning: perf/Linux unavailable on this host ($(uname -s)); " \
         "skipping flamegraph (no SVG produced)." >&2
    exit 0
fi
for s in "$FG_DIR/stackcollapse-perf.pl" "$FG_DIR/flamegraph.pl"; do
    if [[ ! -x "$s" ]]; then
        echo "error: vendored FlameGraph script missing/not executable: $s" >&2
        exit 1
    fi
done

# ---- Hygiene prefix (probe each privilege) ----------------------------------
PREFIX=()
if [[ "${BENCH_RT:-0}" == "1" ]] && chrt -r 99 true 2>/dev/null; then
    PREFIX+=(chrt -r 99)
fi
if [[ -n "${BENCH_CPU:-}" ]] && taskset -c "$BENCH_CPU" true 2>/dev/null; then
    PREFIX+=(taskset -c "$BENCH_CPU")
fi

mkdir -p "$(dirname "$OUT")"
PERF_DATA="$(mktemp -u).perf.data"
trap 'rm -f "$PERF_DATA"' EXIT

echo "recording: perf record -F $FREQ -g -- ${PREFIX[*]} ${ARGS[*]}" >&2
perf record -F "$FREQ" -g -o "$PERF_DATA" -- "${PREFIX[@]}" "${ARGS[@]}"

echo "rendering: $OUT" >&2
perf script -i "$PERF_DATA" \
    | "$FG_DIR/stackcollapse-perf.pl" \
    | "$FG_DIR/flamegraph.pl" --title "$(basename "${ARGS[0]}") flamegraph" \
    > "$OUT"

echo "wrote $OUT" >&2
