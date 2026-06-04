#!/usr/bin/env bash
#
# M11/5 — put a Linux box into a stable benchmarking state (and restore it).
#
# Usage:
#   scripts/linux_bench_env.sh check      # report current state (no changes)
#   sudo scripts/linux_bench_env.sh apply # frequency-lock + perf access + ASLR off
#   sudo scripts/linux_bench_env.sh restore
#
# `apply` does, idempotently:
#   - CPU governor -> performance (all cores)
#   - turbo/boost  -> disabled (intel_pstate/no_turbo=1 or cpufreq/boost=0)
#   - ASLR         -> off  (kernel.randomize_va_space=0)
#   - perf access  -> kernel.perf_event_paranoid=-1, kernel.kptr_restrict=0
#   - NMI watchdog -> off  (kernel.nmi_watchdog=0)
#
# Core isolation (isolcpus=/nohz_full=) is a boot-time parameter and CANNOT
# be set at runtime — `check` reports whether the kernel was booted with it
# and `apply` prints the GRUB line to add. Pin the bench to an isolated
# core with BENCH_CPU=<n> when invoking bench_with_perf.sh /
# bench_flamegraph.sh.
#
# This script only runs on Linux. `apply`/`restore` need root.

set -euo pipefail

ACTION="${1:-check}"

is_linux() { [[ "$(uname -s)" == "Linux" ]]; }
need_root() { if [[ "$(id -u)" -ne 0 ]]; then echo "error: '$ACTION' needs root (use sudo)." >&2; exit 1; fi; }

if ! is_linux; then
    echo "warning: not Linux ($(uname -s)); this script is a no-op here." >&2
    exit 0
fi

write() { # writ(e) value path  — write only if readable/writable, warn otherwise
    local val="$1" path="$2"
    if [[ -w "$path" ]]; then echo "$val" >"$path" && echo "  set $path = $val"
    else echo "  skip $path (not writable)"; fi
}

governors() { ls /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null; }

do_check() {
    echo "== CPU =="
    echo "  model: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
    echo "  cores: $(nproc)"
    local g; g="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
    echo "  governor(cpu0): $g"
    if [[ -r /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo "  intel_pstate/no_turbo: $(cat /sys/devices/system/cpu/intel_pstate/no_turbo)"
    elif [[ -r /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo "  cpufreq/boost: $(cat /sys/devices/system/cpu/cpufreq/boost)"
    else
        echo "  turbo/boost: (no runtime knob found)"
    fi
    echo "== kernel =="
    echo "  randomize_va_space: $(cat /proc/sys/kernel/randomize_va_space 2>/dev/null || echo '?')"
    echo "  perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo '?')"
    echo "  nmi_watchdog: $(cat /proc/sys/kernel/nmi_watchdog 2>/dev/null || echo '?')"
    echo "== isolation (boot-time) =="
    if grep -q 'isolcpus=' /proc/cmdline; then
        echo "  isolcpus: $(tr ' ' '\n' < /proc/cmdline | grep isolcpus=)"
    else
        echo "  isolcpus: NOT set — add 'isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3' to"
        echo "            GRUB_CMDLINE_LINUX and reboot to fully isolate bench cores."
    fi
}

do_apply() {
    need_root
    echo "== applying bench env =="
    if command -v cpupower >/dev/null 2>&1; then
        cpupower frequency-set --governor performance >/dev/null && echo "  cpupower governor=performance"
    else
        local f; for f in $(governors); do echo performance >"$f" 2>/dev/null || true; done
        echo "  governor=performance (via sysfs)"
    fi
    if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        write 1 /sys/devices/system/cpu/intel_pstate/no_turbo
    elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
        write 0 /sys/devices/system/cpu/cpufreq/boost
    fi
    write 0 /proc/sys/kernel/randomize_va_space
    write -1 /proc/sys/kernel/perf_event_paranoid
    write 0 /proc/sys/kernel/kptr_restrict
    write 0 /proc/sys/kernel/nmi_watchdog
    echo "done. Verify with: $0 check"
    if ! grep -q 'isolcpus=' /proc/cmdline; then
        echo "note: cores are NOT isolated at boot; pin with BENCH_CPU=<n> instead."
    fi
}

do_restore() {
    need_root
    echo "== restoring defaults =="
    if command -v cpupower >/dev/null 2>&1; then
        cpupower frequency-set --governor ${RESTORE_GOV:-schedutil} >/dev/null 2>&1 \
            && echo "  governor=${RESTORE_GOV:-schedutil}" || echo "  governor restore skipped"
    fi
    if [[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        write 0 /sys/devices/system/cpu/intel_pstate/no_turbo
    elif [[ -e /sys/devices/system/cpu/cpufreq/boost ]]; then
        write 1 /sys/devices/system/cpu/cpufreq/boost
    fi
    write 2 /proc/sys/kernel/randomize_va_space
    write 2 /proc/sys/kernel/perf_event_paranoid
    write 1 /proc/sys/kernel/nmi_watchdog
    echo "done."
}

case "$ACTION" in
    check)   do_check ;;
    apply)   do_apply ;;
    restore) do_restore ;;
    -h|--help) sed -n '2,28p' "$0" ;;
    *) echo "unknown action: $ACTION (check|apply|restore)" >&2; exit 2 ;;
esac
