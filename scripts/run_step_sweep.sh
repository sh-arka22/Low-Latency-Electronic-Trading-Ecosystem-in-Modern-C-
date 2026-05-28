#!/usr/bin/env bash
# Step-by-step sweep of the v1.2 features. Each variant uses the `full` cfg
# (AS + OFI + hyst + adaptive clip — the prior winner) as its base and adds
# ONE Step's feature on top via env-var overrides. Outputs go to
# data/showcase/BTCUSDT/_step_sweep/pnl_<variant>.csv so the existing
# analyze_pnl.py can summarise them.
#
# Wall-time: ~3-4 hours per variant on BTC full-day tape. The sweep is
# sequential; comment out variants you don't want to re-run.
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=cmake-build-release/backtest_main
TAPE=data/BTCUSDT-2024-03-28.tape
TICK=0.1
OUT=data/showcase/BTCUSDT/_step_sweep
/bin/mkdir -p "$OUT"

# Common positional args for the `full` strategy:
#   label  csv  tape  fmt  tick  tid  clip thr  maxO maxP maxLoss  use_as gamma  kappa  T   use_ofi beta hyst use_aclip sigma_ref
FULL_ARGS=(5 0.5 50 200 -1e9  1 0.05 1.5 6.5    1 0.5 1   1 1.0)

run() {
  local label=$1; shift
  echo ""
  echo "==============================================="
  echo "[sweep] $label"
  echo "==============================================="
  env "$@" "$BIN" "$label" "$OUT/pnl_${label}.csv" "$TAPE" binance "$TICK" 0 "${FULL_ARGS[@]}"
}

# Step 0 — baseline (same as the existing queue-fix-only `full` numbers).
run step0_baseline

# Step 2 — maker rebate booked (+0.5 bp Binance VIP1 rebate).
run step2_rebate \
    MAKER_REBATE_BPS=0.5

# Step 3 — asymmetric OFI spread widening.
run step3_widen \
    SPREAD_WIDEN_OFI=2.0

# Step 4 — killswitch cancel-without-requote.
# Thresholds chosen against the 95th-pctile of |OFI| and (microprice−mid)
# observed in the prior queue-fix-only logs; tune from logs after first run.
run step4_kill \
    USE_KILLSWITCH=1 KILLSWITCH_OFI=200.0 KILLSWITCH_MICRO_TICKS=2.0

# Step 5 — regime-adaptive γ.
run step5_regime \
    USE_REGIME_GAMMA=1 REGIME_GAMMA_SCALE=2.0 EWMA_DECAY_LONG=0.985

# Step 6 — Stoikov state-dependent microprice.
run step6_stoikov \
    USE_STOIKOV_MICRO=1

# Step 7 — VPIN-gated widening + killswitch tightening.
# Bucket size = ~daily_BTC_volume / 100; adjust after first run.
run step7_vpin \
    USE_VPIN=1 VPIN_BUCKET_SIZE=500000 VPIN_THRESHOLD=0.46 \
    VPIN_WIDEN_MULT=2.0 VPIN_KILL_SCALE=0.5

# All-on — every Step 2-7 stacked.
run all_on \
    MAKER_REBATE_BPS=0.5 \
    SPREAD_WIDEN_OFI=2.0 \
    USE_KILLSWITCH=1 KILLSWITCH_OFI=200.0 KILLSWITCH_MICRO_TICKS=2.0 \
    USE_REGIME_GAMMA=1 REGIME_GAMMA_SCALE=2.0 \
    USE_STOIKOV_MICRO=1 \
    USE_VPIN=1 VPIN_BUCKET_SIZE=500000 VPIN_THRESHOLD=0.46 \
    VPIN_WIDEN_MULT=2.0 VPIN_KILL_SCALE=0.5

echo ""
echo "==============================================="
echo "[sweep] DONE — summary follows"
echo "==============================================="
MAKER_REBATE_BOOKED=1 /usr/bin/python3 scripts/analyze_pnl.py "$OUT"/pnl_*.csv \
    | /usr/bin/tee "$OUT/summary.txt"
