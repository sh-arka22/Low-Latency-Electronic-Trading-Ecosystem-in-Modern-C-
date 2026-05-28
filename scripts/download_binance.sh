#!/usr/bin/env bash
# Download one day of Binance USD-M perpetual data from data.binance.vision.
#
# Usage:
#   bash scripts/download_binance.sh                # default: BTCUSDT, yesterday
#   bash scripts/download_binance.sh ETHUSDT 2026-05-01
#
# Produces (in data/raw/<symbol>-<date>/):
#   <symbol>-bookTicker-<date>.csv   (best-bid/ask updates)
#   <symbol>-trades-<date>.csv       (every print)
#
# Next step: bash scripts/merge_binance.py to produce the tape CSV the
# backtest harness consumes.
#
# Source: https://data.binance.vision/?prefix=data/futures/um/daily/
# License: Binance's data is free to use for non-commercial purposes.
set -euo pipefail

cd "$(dirname "$0")/.."

SYMBOL=${1:-BTCUSDT}
# default to yesterday — UTC Y-M-D
DATE=${2:-$(date -u -v-1d +%Y-%m-%d 2>/dev/null || date -u -d "yesterday" +%Y-%m-%d)}

BASE="https://data.binance.vision/data/futures/um/daily"
OUTDIR="data/raw/${SYMBOL}-${DATE}"
mkdir -p "$OUTDIR"

echo "[download] $SYMBOL  $DATE  -> $OUTDIR"

for kind in bookTicker trades; do
  ZIP="${SYMBOL}-${kind}-${DATE}.zip"
  CSV="${SYMBOL}-${kind}-${DATE}.csv"
  URL="${BASE}/${kind}/${SYMBOL}/${ZIP}"

  if [ -f "$OUTDIR/$CSV" ]; then
    echo "  ✓ $kind already present ($OUTDIR/$CSV)"
    continue
  fi
  echo "  ↓ $URL"
  if ! curl -sSfL "$URL" -o "$OUTDIR/$ZIP"; then
    echo "    download failed — common causes:"
    echo "      • date too recent (Binance publishes T+1 UTC; today's file may not exist)"
    echo "      • symbol delisted / not on USD-M perp"
    echo "      • network / firewall"
    exit 2
  fi
  (cd "$OUTDIR" && unzip -q -o "$ZIP" && rm -f "$ZIP")
  ls -la "$OUTDIR/$CSV"
done

echo ""
echo "[download] next:"
echo "  python3 scripts/merge_binance.py $OUTDIR data/${SYMBOL}-${DATE}.tape"
