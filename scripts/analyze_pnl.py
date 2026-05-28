#!/usr/bin/env python3
"""Compute "is this strategy making money" metrics from backtest PnL CSVs.

The pnl_<strategy>.csv emitted by backtest_main has columns:
  ts_ns, mid, bid, ask, position, real_pnl, unreal_pnl, total_pnl,
  volume, num_fills, num_requotes, sigma, ofi

This script reads every pnl_*.csv in cmake-build-release/out/ and prints
a table of:
  gross PnL          — what total_pnl says at the end
  fee-adjusted PnL   — gross minus exchange fees (assumes maker = 0.02%)
  Sharpe (snapshot)  — mean(ΔPnL) / std(ΔPnL) across snapshots, annualized
                       to per-day (the tape is typically one day)
  max drawdown       — biggest peak-to-trough drop in total_pnl
  fill rate          — num_fills / num_requotes
  PnL per fill       — gross / num_fills
  inventory variance — std(position), a risk-not-return metric

Usage:
  python3 scripts/analyze_pnl.py                      # default dir
  python3 scripts/analyze_pnl.py path/to/pnl_*.csv    # explicit files
  MAKER_FEE=0.0002 TAKER_FEE=0.0005 python3 scripts/analyze_pnl.py
"""
from __future__ import annotations
import csv
import glob
import math
import os
import sys

# Binance USD-M perpetual fee tiers (default tier). Override with env vars.
MAKER_FEE = float(os.environ.get("MAKER_FEE", "0.0002"))   # 0.02% (2 bps)
TAKER_FEE = float(os.environ.get("TAKER_FEE", "0.0005"))   # 0.05% (5 bps)
# Assume maker strategies (the only ones here) fill as maker. If you build a
# taker-heavy strategy, set MAKER_FEE=TAKER_FEE=0.0005 to be conservative.

# Step 2 — when the engine books maker rebate/fee live inside PositionKeeper::
# addFill (driven by MAKER_REBATE_BPS), set MAKER_REBATE_BOOKED=1 here so this
# script does not double-charge fees on top of the engine-side accounting.
REBATE_BOOKED = os.environ.get("MAKER_REBATE_BOOKED", "0") != "0"


def stats(xs):
    if not xs:
        return 0.0, 0.0
    m = sum(xs) / len(xs)
    v = sum((x - m) ** 2 for x in xs) / len(xs)
    return m, math.sqrt(v)


def analyze(path: str) -> dict:
    rows = []
    with open(path) as f:
        r = csv.DictReader(f)
        for row in r:
            rows.append({
                "ts_ns":        int(row["ts_ns"]),
                "mid":          float(row["mid"]),
                "position":     int(row["position"]),
                "total_pnl":    float(row["total_pnl"]),
                "volume":       int(row["volume"]),
                "num_fills":    int(row["num_fills"]),
                "num_requotes": int(row["num_requotes"]),
            })
    if not rows:
        return {"strategy": os.path.basename(path), "rows": 0}

    last = rows[-1]
    pnl_series      = [r["total_pnl"] for r in rows]
    pnl_returns     = [b - a for a, b in zip(pnl_series, pnl_series[1:])]
    pos_series      = [r["position"]  for r in rows]

    # Fees. We have *total* contract volume traded — multiply by avg mid and
    # the fee rate to get a fee-cost estimate. Slightly imprecise (uses mid
    # rather than per-fill price) but good enough.
    # When MAKER_REBATE_BOOKED is set, the engine already credited/debited
    # the per-fill rebate into total_pnl_, so fees here would double-count.
    avg_mid = sum(r["mid"] for r in rows) / len(rows)
    notional = last["volume"] * avg_mid
    fees     = 0.0 if REBATE_BOOKED else notional * MAKER_FEE
    fee_adj  = last["total_pnl"] - fees

    # Sharpe — per snapshot, then scale to per-tape (≈1 day if 24h of data).
    mean_ret, sd_ret = stats(pnl_returns)
    sharpe_per_step  = (mean_ret / sd_ret) if sd_ret > 0 else 0.0
    sharpe_full_tape = sharpe_per_step * math.sqrt(len(pnl_returns))

    # Max drawdown.
    peak = pnl_series[0]
    max_dd = 0.0
    for p in pnl_series:
        peak = max(peak, p)
        max_dd = min(max_dd, p - peak)

    return {
        "strategy":   os.path.basename(path).replace("pnl_", "").replace(".csv", ""),
        "rows":       len(rows),
        "gross_pnl":  last["total_pnl"],
        "fees":       fees,
        "fee_adj":    fee_adj,
        "sharpe":     sharpe_full_tape,
        "max_dd":     max_dd,
        "fills":      last["num_fills"],
        "requotes":   last["num_requotes"],
        "fill_rate":  last["num_fills"] / max(last["num_requotes"], 1),
        "pnl/fill":   last["total_pnl"] / max(last["num_fills"], 1),
        "final_pos":  last["position"],
        "pos_std":    stats(pos_series)[1],
        "notional":   notional,
        "avg_mid":    avg_mid,
    }


def main() -> int:
    if len(sys.argv) > 1:
        files = sys.argv[1:]
    else:
        files = sorted(glob.glob("cmake-build-release/out/pnl_*.csv"))
    if not files:
        sys.stderr.write("no PnL CSVs found — run scripts/run_all_strategies.sh first\n")
        return 2

    results = [analyze(p) for p in files]

    print()
    print(f"  Fee model: maker={MAKER_FEE*1e4:.1f}bps, taker={TAKER_FEE*1e4:.1f}bps "
          f"(override with MAKER_FEE / TAKER_FEE env vars)")
    print()
    header = ("strategy", "rows", "gross_pnl", "fees", "fee_adj_pnl",
              "sharpe", "max_dd", "fills", "fill_rate", "pnl/fill",
              "final_pos", "pos_std")
    fmt = ("{strategy:<10} {rows:>6} {gross_pnl:>12.2f} {fees:>10.2f} "
           "{fee_adj:>12.2f} {sharpe:>7.2f} {max_dd:>10.2f} {fills:>6} "
           "{fill_rate:>9.2%} {pnl/fill:>9.4f} {final_pos:>10} {pos_std:>8.1f}")
    print("  " + " ".join(f"{h:>9}" if h != "strategy" else f"{h:<10}" for h in header))
    print("  " + "-" * 130)
    for r in results:
        print("  " + fmt.format(**r))
    print()

    # Pick the winner under fee-adjusted PnL.
    if any(r.get("rows", 0) > 0 for r in results):
        winner = max(results, key=lambda r: r.get("fee_adj", float("-inf")))
        print(f"  WINNER (fee-adjusted): {winner['strategy']:>10}  "
              f"fee_adj_pnl={winner['fee_adj']:.2f}  "
              f"sharpe={winner['sharpe']:.2f}  "
              f"max_dd={winner['max_dd']:.2f}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
