#!/usr/bin/env bash
# Unified sweep: per symbol, runs v1.1 strategies (baseline/as/as_ofi/full)
# plus v1.2 all-on (every toxic-flow / fee / micro-price lever enabled).
#
# Outputs land in data/showcase/<SYMBOL>/pnl_<label>.csv so the analysis
# notebook and analyze_pnl.py pick them up unchanged.
#
# Usage:
#   bash scripts/run_full_sweep.sh
#   SYMBOLS=BTCUSDT bash scripts/run_full_sweep.sh
set -euo pipefail
cd "$(dirname "$0")/.."

DATE=${DATE:-2024-03-28}
SYMBOLS=${SYMBOLS:-"BTCUSDT ETHUSDT SOLUSDT"}
BIN=${BIN:-cmake-build-release/backtest_main}

# Engine debug log otherwise hits 12-18GB and stalls Logger shutdown.
export TRADING_ENGINE_LOG_PATH="${TRADING_ENGINE_LOG_PATH:-/dev/null}"

tick_for() {
  case "$1" in
    BTCUSDT) echo "0.1"   ;;
    ETHUSDT) echo "0.01"  ;;
    SOLUSDT) echo "0.001" ;;
    *) echo "unknown $1" >&2; return 1 ;;
  esac
}

if [[ ! -x "$BIN" ]]; then bash build.sh; fi

# Positional args: clip thr maxO maxP maxLoss use_as gamma kappa T use_ofi beta hyst use_aclip sigma_ref
ARGS_BASELINE=(5 0.5 50 200 -1e9  0 0.05 1.5 6.5  0 0.0 0  0 1.0)
ARGS_AS=(      5 0.5 50 200 -1e9  1 0.05 1.5 6.5  0 0.0 0  0 1.0)
ARGS_AS_OFI=(  5 0.5 50 200 -1e9  1 0.05 1.5 6.5  1 0.5 0  0 1.0)
ARGS_FULL=(    5 0.5 50 200 -1e9  1 0.05 1.5 6.5  1 0.5 1  1 1.0)

# v1.2 env-var levers, calibrated values from run_step_sweep.sh
V12_ENV=(
  MAKER_REBATE_BPS=0.5
  SPREAD_WIDEN_OFI=2.0
  USE_KILLSWITCH=1 KILLSWITCH_OFI=200.0 KILLSWITCH_MICRO_TICKS=2.0
  USE_REGIME_GAMMA=1 REGIME_GAMMA_SCALE=2.0
  USE_STOIKOV_MICRO=1
  USE_VPIN=1 VPIN_BUCKET_SIZE=500000 VPIN_THRESHOLD=0.46
              VPIN_WIDEN_MULT=2.0 VPIN_KILL_SCALE=0.5
)

mkdir -p data/showcase

for SYMBOL in $SYMBOLS; do
  TICK=$(tick_for "$SYMBOL")
  TAPE="data/${SYMBOL}-${DATE}.tape"
  OUT="data/showcase/${SYMBOL}"
  mkdir -p "$OUT"

  if [[ ! -f "$TAPE" ]]; then
    echo "[sweep] preparing tape $TAPE"
    bash scripts/download_binance.sh "$SYMBOL" "$DATE"
    python3 scripts/merge_binance.py "data/raw/${SYMBOL}-${DATE}" "$TAPE"
  fi

  echo ""
  echo "==================================================================="
  echo "[sweep] $SYMBOL  date=$DATE  tick=$TICK"
  echo "==================================================================="

  for variant in baseline as as_ofi full v12_all_on; do
    case "$variant" in
      baseline)   args=("${ARGS_BASELINE[@]}"); env_pre=();;
      as)         args=("${ARGS_AS[@]}");       env_pre=();;
      as_ofi)     args=("${ARGS_AS_OFI[@]}");   env_pre=();;
      full)       args=("${ARGS_FULL[@]}");     env_pre=();;
      v12_all_on) args=("${ARGS_FULL[@]}");     env_pre=("${V12_ENV[@]}");;
    esac
    echo "[run] $SYMBOL $variant"
    env ${env_pre[@]+"${env_pre[@]}"} "$BIN" "$variant" "$OUT/pnl_${variant}.csv" \
        "$TAPE" binance "$TICK" 0 "${args[@]}"
  done
done

echo ""
echo "==================================================================="
echo "[sweep] DONE"
echo "==================================================================="
ls -la data/showcase/*/pnl_*.csv
