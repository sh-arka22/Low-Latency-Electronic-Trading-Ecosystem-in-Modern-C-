#!/usr/bin/env python3
"""Market-making scorecard: the risk-adjusted metrics practitioners actually use
(Falces-Marin/PLOS 2022; Gueant 2012) -- not raw PnL on one path.

Per run: final PnL($), fills, PnL/fill, MAP (max |position|), PnL-to-MAP,
intraday Sharpe, max drawdown($).  MAP/Sharpe/DD reward inventory control.

Usage: mm_scorecard.py <pnl1.csv> [pnl2.csv ...]
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


def score(path):
    d = load(path)
    valid = (d["ask"] < 1e12) & (d["bid"] > 0) & (d["mid"] > 0)
    pos = d["pos"][valid]
    total = d["total"][valid] / 100.0          # USD
    pnl = total[-1]
    fills = d["fills"][valid][-1]
    MAP = max(1.0, float(np.max(np.abs(pos))))  # max absolute inventory (shares)
    dpnl = np.diff(total)
    sharpe = (float(np.mean(dpnl)) / float(np.std(dpnl)) * np.sqrt(len(dpnl))
              if len(dpnl) > 2 and np.std(dpnl) > 0 else float("nan"))
    run_max = np.maximum.accumulate(total)
    maxdd = float(np.max(run_max - total))
    name = os.path.basename(path).replace("pnl_", "").replace(".csv", "")
    return dict(name=name, pnl=pnl, fills=int(fills), ppf=pnl / max(1, fills),
                MAP=MAP, p2map=pnl / MAP, sharpe=sharpe, maxdd=maxdd,
                finalpos=float(pos[-1]))


def main():
    rows = [score(p) for p in sys.argv[1:]]
    hdr = ("run", "PnL$", "fills", "PnL/fill$", "MAP(sh)", "PnL/MAP$",
           "Sharpe", "MaxDD$", "endPos")
    print(f"{hdr[0]:<24}{hdr[1]:>12}{hdr[2]:>9}{hdr[3]:>11}"
          f"{hdr[4]:>10}{hdr[5]:>10}{hdr[6]:>9}{hdr[7]:>12}{hdr[8]:>9}")
    print("-" * 106)
    for r in rows:
        print(f"{r['name']:<24}{r['pnl']:>12,.0f}{r['fills']:>9,}{r['ppf']:>11.2f}"
              f"{r['MAP']:>10,.0f}{r['p2map']:>10.2f}{r['sharpe']:>9.2f}"
              f"{r['maxdd']:>12,.0f}{r['finalpos']:>9,.0f}")


if __name__ == "__main__":
    main()
