#!/usr/bin/env bash
#
# M11/3 — run a bench binary under `perf stat` and emit a hardware-counter
# CSV section that splits with the same `# section: …` convention as the
# bench binaries themselves.
#
# Usage:
#   scripts/bench_with_perf.sh [--perf-csv PATH] -- <binary> [binary args...]
#
# Example (combined CSV: the binary's own sections, then perf_counters):
#   scripts/bench_with_perf.sh -- ./build/release/bench_matching --iters 1000000
#
# The wrapped binary's stdout/stderr pass through untouched (perf writes
# its counters to a temp file via `-o`, which this script parses after the
# run). The perf_counters section is appended to --perf-csv, or stdout.
#
# Benchmarking hygiene (best-effort, each probed before use; the run still
# proceeds if a privilege is unavailable, with a warning):
#   BENCH_CPU=<n>   pin the workload to one core via `taskset -c`
#   BENCH_RT=1      run SCHED_FIFO prio 99 via `chrt -r 99` (needs privilege)
# Frequency lock / ASLR / perf_event_paranoid are the operator's job — see
# scripts/linux_bench_env.sh (M11/5). This script only warns if the CPU
# governor is not `performance`.
#
# Non-Linux or perf-absent hosts: prints a warning and exits 0 (no-op), so
# the script is safe to invoke from any dev box. Real counters require
# Linux perf_events.

set -euo pipefail

PERF_CSV=""
ARGS=()
parsing_self=1
while [[ $# -gt 0 ]]; do
    if [[ $parsing_self -eq 1 ]]; then
        case "$1" in
            --perf-csv) PERF_CSV="$2"; shift 2; continue ;;
            --) parsing_self=0; shift; continue ;;
            -h|--help)
                sed -n '2,40p' "$0"; exit 0 ;;
            *) echo "Unknown wrapper arg: $1 (did you forget '--'?)" >&2; exit 2 ;;
        esac
    else
        ARGS+=("$1"); shift
    fi
done

if [[ ${#ARGS[@]} -eq 0 ]]; then
    echo "error: no binary given. Usage: $0 [--perf-csv PATH] -- <binary> [args...]" >&2
    exit 2
fi

emit() { if [[ -n "$PERF_CSV" ]]; then echo "$1" >>"$PERF_CSV"; else echo "$1"; fi; }

# ---- Host capability gate ---------------------------------------------------
if [[ "$(uname -s)" != "Linux" ]] || ! command -v perf >/dev/null 2>&1; then
    echo "warning: perf/Linux unavailable on this host ($(uname -s)); " \
         "running the binary WITHOUT hardware counters (no-op section)." >&2
    "${ARGS[@]}"
    emit "# section: perf_counters"
    emit "counter,value"
    emit "available,0"
    exit 0
fi

# ---- Governor sanity check (warn only) --------------------------------------
gov_file=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [[ -r "$gov_file" ]]; then
    gov="$(cat "$gov_file")"
    if [[ "$gov" != "performance" ]]; then
        echo "warning: CPU governor is '$gov', not 'performance' — numbers" \
             "will be noisy. See scripts/linux_bench_env.sh." >&2
    fi
fi

# ---- Build the hygiene prefix (probe each privilege) ------------------------
PREFIX=()
if [[ "${BENCH_RT:-0}" == "1" ]]; then
    if chrt -r 99 true 2>/dev/null; then
        PREFIX+=(chrt -r 99)
    else
        echo "warning: BENCH_RT=1 but chrt -r 99 needs privilege; not using SCHED_FIFO." >&2
    fi
fi
if [[ -n "${BENCH_CPU:-}" ]]; then
    if taskset -c "$BENCH_CPU" true 2>/dev/null; then
        PREFIX+=(taskset -c "$BENCH_CPU")
    else
        echo "warning: BENCH_CPU=$BENCH_CPU set but taskset failed; not pinning." >&2
    fi
fi

EVENTS="${PERF_EVENTS:-cycles,instructions,branches,branch-misses,cache-references,cache-misses,LLC-loads,LLC-load-misses,page-faults}"

PERF_OUT="$(mktemp)"
trap 'rm -f "$PERF_OUT"' EXIT

# `-x ,` => machine-readable CSV; `-o FILE` keeps it off the binary's streams.
set +e
perf stat -x , -o "$PERF_OUT" -e "$EVENTS" -- "${PREFIX[@]}" "${ARGS[@]}"
perf_rc=$?
set -e

# ---- Parse perf's CSV into a `# section: perf_counters` block ---------------
# perf -x , line: value,unit,event,runtime,pct,[metric,metric-unit]
emit "# section: perf_counters"
emit "counter,value"
emit "available,1"
emit "perf_exit_code,$perf_rc"

awk -F, '
    /^#/ { next }
    NF < 3 { next }
    {
        val=$1; ev=$3;
        gsub(/^[ \t]+|[ \t]+$/, "", val);
        gsub(/^[ \t]+|[ \t]+$/, "", ev);
        if (ev == "") next;
        gsub(/[ \t]/, "_", ev);
        print ev "," val;
        if (ev == "instructions") instr=val;
        if (ev == "cycles") cyc=val;
        if (ev == "branches") br=val;
        if (ev == "branch-misses" || ev == "branch_misses") brm=val;
        if (ev == "cache-references" || ev == "cache_references") cref=val;
        if (ev == "cache-misses" || ev == "cache_misses") cmiss=val;
    }
    END {
        if (cyc+0 > 0 && instr+0 > 0) printf "ipc,%.4f\n", instr/cyc;
        if (br+0 > 0 && brm+0 > 0)    printf "branch_miss_pct,%.4f\n", 100*brm/br;
        if (cref+0 > 0 && cmiss+0 > 0) printf "cache_miss_pct,%.4f\n", 100*cmiss/cref;
    }
' "$PERF_OUT" | while IFS= read -r line; do emit "$line"; done

emit ""
exit "$perf_rc"
