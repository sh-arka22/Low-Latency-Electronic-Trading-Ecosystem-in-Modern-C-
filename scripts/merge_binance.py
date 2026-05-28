#!/usr/bin/env python3
"""Merge Binance USD-M perp bookTicker + trades CSVs into the project's tape format.

Input: a directory containing
  <SYMBOL>-bookTicker-<DATE>.csv  columns: update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time
  <SYMBOL>-trades-<DATE>.csv      columns: id,price,qty,quote_qty,time,is_buyer_maker

Output: a single time-sorted CSV the backtest harness can read, with schema
  ts_ns,kind,price,qty,bid_price,bid_qty,ask_price,ask_qty,is_buyer_maker
where kind = "B" for book updates (uses bid/ask cols) or "T" for trades.

Usage:
  python3 scripts/merge_binance.py data/raw/BTCUSDT-2026-05-01 data/BTCUSDT-2026-05-01.tape

Then:
  bash scripts/run_all_strategies.sh data/BTCUSDT-2026-05-01.tape binance 0.1
"""
from __future__ import annotations
import csv
import glob
import os
import sys


def find_one(d: str, pattern: str) -> str:
    matches = glob.glob(os.path.join(d, pattern))
    if not matches:
        sys.stderr.write(f"merge_binance: no file matching {pattern} in {d}\n")
        sys.exit(2)
    return matches[0]


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: merge_binance.py <raw_dir> <output_tape.csv>\n")
        return 2
    raw_dir, out_path = sys.argv[1], sys.argv[2]

    book_csv  = find_one(raw_dir, "*-bookTicker-*.csv")
    trade_csv = find_one(raw_dir, "*-trades-*.csv")

    events = []  # (ts_ns, row_tuple)

    print(f"[merge] reading {book_csv}")
    with open(book_csv) as f:
        r = csv.DictReader(f)
        for row in r:
            # Binance gives event_time in milliseconds; convert to nanos.
            ts_ms = int(row["event_time"])
            ts_ns = ts_ms * 1_000_000
            bp = row["best_bid_price"]
            bq = row["best_bid_qty"]
            ap = row["best_ask_price"]
            aq = row["best_ask_qty"]
            # kind=B: bid/ask cols populated, price/qty empty
            events.append((ts_ns, ("B", "", "", bp, bq, ap, aq, "")))

    print(f"[merge] reading {trade_csv}")
    with open(trade_csv) as f:
        r = csv.DictReader(f)
        for row in r:
            ts_ms = int(row["time"])
            ts_ns = ts_ms * 1_000_000
            p   = row["price"]
            q   = row["qty"]
            ibm = "1" if row["is_buyer_maker"].lower() in ("true", "1") else "0"
            # kind=T: price/qty populated, bid/ask empty
            events.append((ts_ns, ("T", p, q, "", "", "", "", ibm)))

    print(f"[merge] sorting {len(events):,} events by ts_ns")
    events.sort(key=lambda r: r[0])

    print(f"[merge] writing {out_path}")
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_ns", "kind", "price", "qty",
                    "bid_price", "bid_qty", "ask_price", "ask_qty",
                    "is_buyer_maker"])
        for ts_ns, vals in events:
            kind, p, q, bp, bq, ap, aq, ibm = vals
            w.writerow([ts_ns, kind, p, q, bp, bq, ap, aq, ibm])

    print(f"[merge] done — {len(events):,} rows, "
          f"{os.path.getsize(out_path)/1024/1024:.1f} MB")
    print(f"[merge] next:")
    print(f"  bash scripts/run_all_strategies.sh {out_path} binance 0.1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
