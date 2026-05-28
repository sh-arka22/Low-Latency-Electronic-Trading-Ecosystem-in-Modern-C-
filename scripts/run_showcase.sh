#!/usr/bin/env bash
# Resume showcase: download one day of real Binance USD-M perp data for three
# symbols, merge into the project tape format, and sweep the 4-strategy
# variant (baseline / as / as_ofi / full) over each. All 12 PnL CSVs land
# under data/showcase/<SYMBOL>/ so the analysis notebook can pick them up.
#
# Usage:
#   bash scripts/run_showcase.sh
#   DATE=2026-05-12 bash scripts/run_showcase.sh
#   SYMBOLS="BTCUSDT" bash scripts/run_showcase.sh   # single-symbol smoke test
#
# Defaults to 2026-05-15 (a recent Friday — Binance Vision publishes T+1 UTC,
# so any date older than two days is safe).
set -euo pipefail

cd "$(dirname "$0")/.."

## Binance retired the public `bookTicker` daily archive on 2024-03-30. Newer
## dates still have `trades` + `bookDepth` (L2) but not the L1 stream this
## tape reader consumes — so we anchor the showcase to the last full mid-week
## trading day before retirement (Thu 2024-03-28). All three symbols are
## verified available on that date.
DATE=${DATE:-2024-03-28}
SYMBOLS=${SYMBOLS:-"BTCUSDT ETHUSDT SOLUSDT"}

# Per-symbol tick size on Binance USD-M perp. If you add a new symbol, look up
# its priceTickSize at https://fapi.binance.com/fapi/v1/exchangeInfo and add
# the row here.
tick_for() {
  case "$1" in
    BTCUSDT) echo "0.1"   ;;
    ETHUSDT) echo "0.01"  ;;
    SOLUSDT) echo "0.001" ;;   # SOL's real Binance tick — NOT 0.01. With
                                # 0.01, bid (e.g. 186.446) and ask (186.447)
                                # round to the same integer tick, collapsing
                                # both sides into one price level and
                                # triggering cross-side corruption in
                                # MarketOrderBook (silent ADD into wrong-side
                                # chain → null deref in addOrdersAtPrice).
    *)
      echo "unknown symbol $1 — add it to tick_for() in $0" >&2
      return 1
      ;;
  esac
}

BIN=${BIN:-cmake-build-release/backtest_main}
if [[ ! -x "$BIN" ]]; then
  echo "[showcase] building backtest_main..."
  bash build.sh
fi

mkdir -p data/showcase
START_NS=$(python3 -c 'import time; print(int(time.time()*1e9))')

for SYMBOL in $SYMBOLS; do
  TICK=$(tick_for "$SYMBOL")
  RAW_DIR="data/raw/${SYMBOL}-${DATE}"
  TAPE="data/${SYMBOL}-${DATE}.tape"
  OUT="data/showcase/${SYMBOL}"

  echo ""
  echo "==================================================================="
  echo "[showcase] $SYMBOL  date=$DATE  tick=$TICK  out=$OUT"
  echo "==================================================================="

  # Step 1: download (idempotent — download_binance.sh skips if CSVs present).
  bash scripts/download_binance.sh "$SYMBOL" "$DATE"

  # Step 2: merge into the unified tape format. Skip if already merged.
  if [[ -f "$TAPE" ]]; then
    echo "[showcase] tape already merged: $TAPE"
  else
    python3 scripts/merge_binance.py "$RAW_DIR" "$TAPE"
  fi

  # Step 3: run the 4-strategy sweep, dropping outputs into per-symbol dir.
  mkdir -p "$OUT"
  OUTDIR="$OUT" bash scripts/run_all_strategies.sh "$TAPE" binance "$TICK"

  # Step 4: summarize this symbol so the wall-time numbers are visible live.
  echo ""
  echo "[showcase] $SYMBOL summary:"
  MAKER_FEE=${MAKER_FEE:-0.0002} python3 scripts/analyze_pnl.py "$OUT"/pnl_*.csv \
    | tee "$OUT/summary.txt"
done

END_NS=$(python3 -c 'import time; print(int(time.time()*1e9))')
ELAPSED_S=$(python3 -c "print(f'{($END_NS - $START_NS)/1e9:.1f}')")

echo ""
echo "==================================================================="
echo "[showcase] DONE in ${ELAPSED_S}s"
echo "==================================================================="
echo "Per-symbol outputs:"
for SYMBOL in $SYMBOLS; do
  echo "  data/showcase/${SYMBOL}/"
  ls -la "data/showcase/${SYMBOL}/" 2>/dev/null | sed 's/^/    /'
done
echo ""
echo "Next:"
echo "  jupyter nbconvert --execute notebooks/showcase_analysis.ipynb"
echo "  open data/showcase/pnl_curves.png data/showcase/sharpe_fillrate.png"
