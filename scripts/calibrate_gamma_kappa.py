#!/usr/bin/env python3
"""γ / κ calibration helper for the Avellaneda-Stoikov spread formula.

The AS κ-term contributes (2/γ)·ln(1 + γ/κ) of fixed width to the spread.
Given a desired *minimum* half-spread (in ticks) and an empirically estimated
κ (order-arrival intensity), this script solves for γ. Then it sanity-checks
the σ-term contribution against the spread distribution in a pnl_*.csv.

Usage:
  scripts/calibrate_gamma_kappa.py data/showcase/BTCUSDT/pnl_full.csv \\
      --tick 0.1 --min-half-spread-ticks 2

Outputs a recommended γ + the κ used and a short rationale.
Source: hftbacktest GLFT tutorial; Guéant 2017 (Applied Math Finance).
"""
from __future__ import annotations
import argparse
import csv
import math
import sys


def fit_kappa_from_fills(csv_path: str) -> tuple[float, float]:
    """Crude κ estimate: fill rate per snapshot / spread-width-in-ticks.

    κ in the AS model is the *arrival intensity* of orders that fill our
    quote. Higher κ → tighter optimal spread. We estimate via the simple
    rule of thumb used in practitioner notes: κ ≈ fills_per_second.
    Returns (kappa_est, avg_spread_ticks).
    """
    fills = []
    spreads = []
    ts = []
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            ts.append(int(row["ts_ns"]))
            fills.append(int(row["num_fills"]))
            spreads.append(int(row["ask"]) - int(row["bid"]))
    if len(ts) < 2:
        sys.exit("not enough rows in csv to calibrate")
    duration_s = (ts[-1] - ts[0]) / 1e9
    fills_total = fills[-1] - fills[0]
    fills_per_s = fills_total / max(duration_s, 1.0)
    avg_spread = sum(spreads) / len(spreads)
    return fills_per_s, avg_spread


def gamma_from_min_half_spread(target_half_spread_price: float,
                                kappa: float,
                                gamma_lo: float = 1e-5,
                                gamma_hi: float = 10.0) -> float:
    """Solve (2/γ)·ln(1 + γ/κ) ≈ 2·target_half_spread by bisection.

    The σ-term `γ·σ²·τ` is ignored here (it adds further width on top); the
    κ-term is what dominates at the *minimum* spread the formula will ever
    emit (when σ is small or τ near 0).
    """
    target = 2.0 * target_half_spread_price
    def f(g):
        return (2.0 / g) * math.log1p(g / kappa) - target
    # f is decreasing in γ; find the γ that drives f(γ)=0.
    lo, hi = gamma_lo, gamma_hi
    if f(lo) <= 0:
        return lo
    if f(hi) >= 0:
        return hi
    for _ in range(80):
        mid = math.sqrt(lo * hi)  # geometric bisection for scale invariance
        if f(mid) > 0:
            lo = mid
        else:
            hi = mid
    return math.sqrt(lo * hi)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("pnl_csv", help="A pnl_*.csv emitted by backtest_main")
    p.add_argument("--tick", type=float, required=True,
                   help="Tick size in price units (e.g. 0.1 for BTCUSDT)")
    p.add_argument("--min-half-spread-ticks", type=float, default=2.0,
                   help="Desired AS κ-term floor in ticks (each side). "
                        "Default 2 — Binance crypto practical minimum.")
    args = p.parse_args()

    kappa, avg_spread_price = fit_kappa_from_fills(args.pnl_csv)
    target_half = args.min_half_spread_ticks * args.tick
    gamma = gamma_from_min_half_spread(target_half, kappa)

    kterm = (2.0 / gamma) * math.log1p(gamma / kappa)
    print(f"  Calibration from {args.pnl_csv}")
    print(f"  κ_est (fills/sec):           {kappa:.4f}")
    print(f"  observed avg spread (price): {avg_spread_price:.3f} "
          f"(~{avg_spread_price/args.tick:.1f} ticks)")
    print(f"  target half-spread floor:    {args.min_half_spread_ticks} ticks "
          f"= {target_half:.3f} price")
    print(f"  → recommended γ:             {gamma:.5f}")
    print(f"    AS κ-term @ this γ:        {kterm:.3f} price "
          f"(~{kterm/args.tick:.1f} ticks)")
    print()
    print(f"  Wire it via env var to run_all_strategies.sh or backtest_main:")
    print(f"    GAMMA={gamma:.5f} bash scripts/run_showcase.sh   # or edit CLI arg 13")
    return 0


if __name__ == "__main__":
    sys.exit(main())
