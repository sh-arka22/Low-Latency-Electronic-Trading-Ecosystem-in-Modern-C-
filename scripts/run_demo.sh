#!/usr/bin/env bash
# Day 1 demo runner — exchange + one MAKER client for 30 s, SIGINT, gather artefacts.
# Produces:
#   latency_<tag>.hgrm    — per-tag latency histograms dumped by TradeEngine
#   trading_engine_1.log  — TTT + RDTSC lines (Ch11 instrumentation)
#   docs/latency.png      — percentile plot (via scripts/plot.py)
set -euo pipefail

cd "$(dirname "$0")/.."
BIN_EXCHANGE=${BIN_EXCHANGE:-cmake-build-release/exchange_main}
BIN_TRADING=${BIN_TRADING:-cmake-build-release/trading_main}

if [[ ! -x "$BIN_EXCHANGE" || ! -x "$BIN_TRADING" ]]; then
  echo "[run_demo] building..."
  bash build.sh
fi

mkdir -p docs

# Clean any leftover artefacts.
rm -f latency_*_*.hgrm exchange_*.log trading_*.log

echo "[run_demo] starting exchange_main"
"$BIN_EXCHANGE" >/dev/null 2>&1 &
EXCH_PID=$!

# Exchange needs ~10 s for OrderServer to listen (per ch11 memory).
sleep 12

echo "[run_demo] starting RANDOM client (client_id=1) for order flow (v1.0 5-field CLI)"
# Per-ticker (v1.0 stride): clip threshold max_order max_position max_loss
RPER='100 0.5 100 1000 -1e9'
"$BIN_TRADING" 1 RANDOM \
  $RPER $RPER $RPER $RPER $RPER $RPER $RPER $RPER >/dev/null 2>&1 &
RAND_PID=$!

# Brief stagger so the RANDOM client's OG is connected before MAKER joins.
sleep 1

echo "[run_demo] starting MAKER client (client_id=2) with full v1.1 flags"
# Per-ticker (v1.1 stride): clip thr maxO maxP maxLoss  use_as γ κ T   use_ofi β hyst use_aclip σ_ref
MPER='100 0.5 100 1000 -1e9  1 0.1 1.5 6.5  1 0.5 1 1 1.0'
"$BIN_TRADING" 2 MAKER \
  $MPER $MPER $MPER $MPER $MPER $MPER $MPER $MPER >/dev/null 2>&1 &
TR_PID=$!

echo "[run_demo] running for 30 s..."
sleep 30

echo "[run_demo] SIGINT trading + exchange"
kill -INT $TR_PID  $RAND_PID 2>/dev/null || true
sleep 5
kill -INT $EXCH_PID 2>/dev/null || true
sleep 10

echo ""
echo "=== histograms produced ==="
ls -la latency_2_*.hgrm 2>/dev/null | head -10 || echo "(none — likely build/start race)"

echo ""
echo "=== percentile summary ==="
for f in latency_2_*.hgrm; do
  [ -f "$f" ] || continue
  c=$(grep '^# count,'   "$f" | cut -d, -f2)
  p50=$(grep '^# p50,'   "$f" | cut -d, -f2)
  p99=$(grep '^# p99,'   "$f" | cut -d, -f2)
  p999=$(grep '^# p999,' "$f" | cut -d, -f2)
  name=$(grep '^# tag,'  "$f" | cut -d, -f2)
  printf "  %-50s count=%-8s p50=%-8s p99=%-10s p999=%s\n" "$name" "$c" "$p50" "$p99" "$p999"
done

echo ""
echo "[run_demo] generating docs/latency.png"
if command -v python3 >/dev/null 2>&1; then
  python3 scripts/plot.py latency || echo "[run_demo] plot.py failed (likely missing matplotlib) — install with: pip install matplotlib"
else
  echo "[run_demo] python3 not on PATH — skipping plot"
fi

echo ""
echo "[run_demo] done. Inspect: latency_2_*.hgrm, docs/latency.png"
