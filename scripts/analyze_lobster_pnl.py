#!/usr/bin/env python3
"""Correlate LOBSTER L3 backtest PnL series against EXPECTED microstructure relationships.

Checks, per run:
  - book integrity      : 0 crossed rows (bid<ask), and bid <= mid <= ask always
  - real price path     : open/high/low/close + day return (should match the real tape)
  - adverse selection   : corr(position, mid)   -> NEGATIVE on a down day (maker accumulates
                          inventory against the move)
  - PnL consistency     : corr(unreal_pnl, mid) -> POSITIVE when net long (mark-to-market)
  - monotonic fills/volume
Cross-run:
  - same symbol, two strategies -> mid paths must be ~identical (same tape) => corr ~ 1.0

Usage: analyze_lobster_pnl.py <pnl1.csv> [pnl2.csv ...]
"""
import csv
import os
import sys

import numpy as np

COLS = ["ts", "mid", "bid", "ask", "pos", "real", "unreal",
        "total", "vol", "fills", "requotes", "sigma", "ofi"]


def load(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) < 13:
                continue
            try:
                rows.append([float(x) for x in row[:13]])
            except ValueError:
                continue
    a = np.array(rows, dtype=float)
    return {c: a[:, i] for i, c in enumerate(COLS)}


def corr(x, y):
    m = np.isfinite(x) & np.isfinite(y)
    if m.sum() < 3 or np.std(x[m]) == 0 or np.std(y[m]) == 0:
        return float("nan")
    return float(np.corrcoef(x[m], y[m])[0, 1])


def main():
    mids = {}
    for path in sys.argv[1:]:
        d = load(path)
        valid = (d["ask"] < 1e12) & (d["bid"] > 0) & (d["mid"] > 0)
        mid_c = d["mid"][valid]
        mid = mid_c / 100.0
        bid, ask = d["bid"][valid], d["ask"][valid]
        pos = d["pos"][valid]
        unreal = d["unreal"][valid] / 100.0
        total = d["total"][valid] / 100.0
        name = os.path.basename(path).replace("pnl_", "").replace(".csv", "")
        mids[name] = mid

        crossed = int(np.sum(bid >= ask))
        micro_ok = bool(np.all((bid <= mid_c) & (mid_c <= ask)))
        print(f"== {name} ==")
        print(f"   rows={len(mid)}  crossed(bid>=ask)={crossed}  bid<=mid<=ask always={micro_ok}")
        print(f"   mid$  open={mid[0]:.2f} high={mid.max():.2f} low={mid.min():.2f} "
              f"close={mid[-1]:.2f}  day_return={100 * (mid[-1] / mid[0] - 1):+.2f}%")
        print(f"   final pos={d['pos'][-1]:.0f}  fills={d['fills'][-1]:.0f}  "
              f"total_pnl=${total[-1]:,.0f}  (real=${d['real'][-1] / 100:,.0f}, "
              f"unreal=${unreal[-1]:,.0f})")
        print(f"   corr(position, mid)   = {corr(pos, mid):+.3f}   "
              f"[adverse selection -> NEGATIVE on a down day]")
        print(f"   corr(unreal_pnl, mid) = {corr(unreal, mid):+.3f}   "
              f"[net-long into falling mid -> POSITIVE]")
        print(f"   fills monotonic nondecreasing = {bool(np.all(np.diff(d['fills']) >= 0))}")
        print()

    # Same-symbol strategies share the tape => mid paths must be ~identical.
    names = list(mids)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            sym_i = names[i].split("_")[0]
            sym_j = names[j].split("_")[0]
            if sym_i != sym_j:
                continue
            a, b = mids[names[i]], mids[names[j]]
            n = min(len(a), len(b))
            print(f"   corr(mid: {names[i]} vs {names[j]}) = {corr(a[:n], b[:n]):+.5f}  "
                  f"[same tape -> expect ~1.0]")


if __name__ == "__main__":
    main()
