#!/usr/bin/env bash
# Run all four strategy variants against the same synthetic tape (or a real
# Binance tape if provided) and emit one PnL CSV per strategy.
#
# Usage:
#   scripts/run_all_strategies.sh
#   scripts/run_all_strategies.sh path/to/tape.csv binance 0.1
set -euo pipefail

TAPE_PATH="${1:-unused}"
FORMAT="${2:-synth}"
TICK="${3:-1}"

cd "$(dirname "$0")/.."
BIN=${BIN:-cmake-build-release/backtest_main}
OUTDIR=${OUTDIR:-cmake-build-release/out}
mkdir -p "$OUTDIR"

# Per-event engine debug log can hit 12-18GB on a full-day Binance tape and
# stall the Logger destructor for hours. Backtests don't need it — the PnL
# CSV is the deliverable. Send the engine log to /dev/null unless the caller
# explicitly overrides.
export TRADING_ENGINE_LOG_PATH="${TRADING_ENGINE_LOG_PATH:-/dev/null}"


if [[ ! -x "$BIN" ]]; then
  echo "Building backtest_main..."
  bash build.sh
fi

#         label        csv                       tape       fmt    tick tid clip thr maxO maxP maxLoss  use_as gamma  kappa  T   use_ofi beta hyst use_aclip sigma_ref
run() {
  echo "[run] $1"
  "$BIN" "$@"
}

run baseline "$OUTDIR/pnl_baseline.csv"   "$TAPE_PATH" "$FORMAT" "$TICK" 0 \
    5 0.5 50 200 -1e9  0 0.05 1.5 6.5    0 0.0 0   0 1.0
run as       "$OUTDIR/pnl_as.csv"         "$TAPE_PATH" "$FORMAT" "$TICK" 0 \
    5 0.5 50 200 -1e9  1 0.05 1.5 6.5    0 0.0 0   0 1.0
run as_ofi   "$OUTDIR/pnl_as_ofi.csv"     "$TAPE_PATH" "$FORMAT" "$TICK" 0 \
    5 0.5 50 200 -1e9  1 0.05 1.5 6.5    1 0.5 0   0 1.0
run full     "$OUTDIR/pnl_full.csv"       "$TAPE_PATH" "$FORMAT" "$TICK" 0 \
    5 0.5 50 200 -1e9  1 0.05 1.5 6.5    1 0.5 1   1 1.0

echo "Done. Outputs in $OUTDIR/"
ls -la "$OUTDIR"/pnl_*.csv
