#!/usr/bin/env bash
# M7 done-when verification: assert that the release-build templated
# MarketMakerT<S,L> hot path has no indirect-call instructions (vtable
# dispatch) inside the [[gnu::noinline]] tick_once symbols defined in
# bench/bench_engine.cpp.
#
# Direct calls (bl <symbol> on arm64, callq <addr> on x86_64) are fine —
# those are statically-resolved calls. Only register-indirect calls
# (blr/br on arm64, callq *... on x86_64) indicate virtual dispatch.
#
# Usage: check_no_virtual_dispatch.sh <bench_engine_binary>

set -euo pipefail

BIN="${1:-}"
if [[ -z "$BIN" || ! -x "$BIN" ]]; then
    echo "ERROR: bench_engine binary not provided or not executable: '$BIN'" >&2
    exit 1
fi

uname_s="$(uname -s)"
case "$uname_s" in
    Darwin)
        DISASM=(otool -tV "$BIN")
        ARCH="$(uname -m)"
        ;;
    Linux)
        if ! command -v objdump >/dev/null 2>&1; then
            echo "PASS (skipped): objdump not available on this Linux host"
            exit 0
        fi
        DISASM=(objdump -d --no-show-raw-insn -C "$BIN")
        ARCH="$(uname -m)"
        ;;
    *)
        echo "PASS (skipped): unrecognized platform '$uname_s'; not gating M7 check here"
        exit 0
        ;;
esac

# Indirect-call instruction patterns by ISA. Word-boundaries so we don't
# match `bl ` or `call ` (the direct forms).
case "$ARCH" in
    arm64|aarch64)
        # arm64: register-indirect call = `blr x?`; register-indirect tail
        # call = `br x?`. We exclude `br.cond` / `b.cond` forms which are
        # conditional direct branches, not indirect.
        INDIRECT_RE='\b(blr|br)[[:space:]]+x[0-9]+'
        ;;
    x86_64|amd64)
        # x86_64 (AT&T syntax via objdump/otool): indirect call/jmp has a
        # `*` prefix on the operand: `callq *%rax`, `jmpq *0x10(%rbx)`.
        INDIRECT_RE='\b(callq?|jmpq?)[[:space:]]+\*'
        ;;
    *)
        echo "PASS (skipped): unrecognized arch '$ARCH'; not gating M7 check here"
        exit 0
        ;;
esac

DUMP="$(mktemp)"
trap 'rm -f "$DUMP"' EXIT
"${DISASM[@]}" > "$DUMP" 2>/dev/null || {
    echo "ERROR: disassembler failed on '$BIN'" >&2
    exit 1
}

# Demangle if c++filt is available — improves grep readability for
# symbols like `_ZL9tick_onceI17HeuristicStrategy...`. otool -tV on
# macOS doesn't auto-demangle.
if command -v c++filt >/dev/null 2>&1; then
    c++filt < "$DUMP" > "${DUMP}.dem"
    mv "${DUMP}.dem" "$DUMP"
fi

# A function label in objdump/otool output is a line ending in `:` —
# after c++filt the symbol may contain spaces (e.g. "void tick_once<..."),
# so we anchor only on the trailing colon. Instruction lines start with
# whitespace (an offset/address); label lines do not.
LABEL_LINES="$(grep -nE '^[^[:space:]].*:[[:space:]]*$' "$DUMP" | cut -d: -f1)"
TICK_STARTS="$(grep -nE '^[^[:space:]].*tick_once.*:[[:space:]]*$' "$DUMP" | cut -d: -f1)"

if [[ -z "$TICK_STARTS" ]]; then
    echo "ERROR: no tick_once<...> symbols found in $BIN — was bench_engine built with the [[gnu::noinline]] wrapper?" >&2
    echo "       hint: rebuild --preset release after the M7/3 commit" >&2
    exit 1
fi

FAIL=0
COUNT=0
for start in $TICK_STARTS; do
    COUNT=$((COUNT + 1))
    # Find the next label line strictly after `start`.
    end=""
    for L in $LABEL_LINES; do
        if [[ "$L" -gt "$start" ]]; then
            end="$L"
            break
        fi
    done
    if [[ -z "$end" ]]; then
        end="$(wc -l < "$DUMP")"
    fi
    label="$(sed -n "${start}p" "$DUMP")"
    body="$(sed -n "$((start+1)),$((end-1))p" "$DUMP")"
    hits="$(printf '%s\n' "$body" | grep -E "$INDIRECT_RE" || true)"
    if [[ -n "$hits" ]]; then
        echo "FAIL: indirect call(s) found in $label" >&2
        printf '%s\n' "$hits" | sed 's/^/    /' >&2
        FAIL=1
    else
        size=$((end - start - 1))
        echo "OK: $label — $size lines, 0 indirect calls"
    fi
done

if [[ "$FAIL" -ne 0 ]]; then
    echo "M7 no-virtual-dispatch check FAILED" >&2
    exit 1
fi

echo "M7 no-virtual-dispatch check passed (${COUNT} tick_once instantiation(s) verified)"
