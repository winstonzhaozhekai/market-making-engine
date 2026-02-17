#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

echo "=== Building benchmark (release) ==="
make bench

EVENTS="${1:-10000}"
SEED="${2:-42}"

echo ""
echo "=== Running benchmark: $EVENTS events, seed=$SEED ==="
echo ""
./bench/bench_engine --events "$EVENTS" --seed "$SEED"

echo ""
echo "=== Determinism check ==="
make tests/test_determinism
./tests/test_determinism

echo ""
echo "=== Done ==="
