#!/usr/bin/env bash
#
# M11/6 — fetch a real Nasdaq TotalView-ITCH 5.0 sample tape for the
# honest backtest the README publishes.
#
# Nasdaq publishes full trading-day samples at:
#   https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/<MMDDYYYY>.NASDAQ_ITCH50.gz
# Files are multi-GB compressed (tens of GB uncompressed) — they are NOT
# committed; this script downloads into data/itch/ (gitignored) and
# gunzips. The backtest report CSV (small) IS committed by run_m11_suite.sh.
#
# Default: 2019-10-15 — a quiet pre-pandemic, pre-meme-stock Tuesday, a
# representative "boring" session for a credible baseline.
#
# Usage:
#   scripts/download_sample_tape.sh [--date MMDDYYYY] [--keep-gz]
#
# Needs: curl, gunzip, ~15-20 GB free in data/itch/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEST="$PROJECT_DIR/data/itch"

DATE="10152019"   # 2019-10-15
KEEP_GZ=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --date) DATE="$2"; shift 2 ;;
        --keep-gz) KEEP_GZ=1; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DATE}.NASDAQ_ITCH50.gz"
GZ="$DEST/${DATE}.NASDAQ_ITCH50.gz"
RAW="$DEST/${DATE}.NASDAQ_ITCH50"

mkdir -p "$DEST"

if [[ -f "$RAW" ]]; then
    echo "already present: $RAW ($(du -h "$RAW" | cut -f1))"
    exit 0
fi

echo "downloading $URL" >&2
echo "  -> $GZ  (multi-GB; this takes a while)" >&2
curl -fL --retry 3 -o "$GZ" "$URL"

echo "gunzipping -> $RAW" >&2
if [[ "$KEEP_GZ" -eq 1 ]]; then
    gunzip -k -f "$GZ"
else
    gunzip -f "$GZ"
fi

echo "done: $RAW ($(du -h "$RAW" | cut -f1))" >&2
echo "next: scripts/run_m11_suite.sh --tape $RAW --symbol AAPL --cpu 2 --rt" >&2
