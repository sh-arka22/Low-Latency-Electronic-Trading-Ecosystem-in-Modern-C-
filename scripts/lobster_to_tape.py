#!/usr/bin/env python3
"""Convert a LOBSTER (NASDAQ L3) message+orderbook pair into the project's .tape format.

This lets the EXISTING backtest harness (binance_tape_reader, format="binance") replay real
NASDAQ order-by-order data with zero C++ changes. It consumes the L3 feed at *top-of-book*
(level-1 best bid/ask + executions) — full-depth L3 requires the order-book price-index rework
(market_order_book.h priceToIndex = price % ME_MAX_PRICE_LEVELS), which is a separate change.

LOBSTER message columns:  Time, Type, OrderID, Size, Price, Direction
  Time      : seconds after midnight (e.g. 34200.0 = 09:30:00)
  Type      : 1=new LO  2=partial-cancel  3=delete  4=visible-exec  5=hidden-exec  6=cross  7=halt
  Size      : shares
  Price     : integer dollars x 1e4   (5853300 = $585.33)
  Direction : +1 = buy limit order, -1 = sell limit order (for execs: the side of the LO executed)

LOBSTER orderbook columns (level N): AskP1,AskS1,BidP1,BidS1, AskP2,... (4*N), prices x1e4, sizes shares.

Output .tape schema (read by binance_tape_reader with format=binance, tick_size=0.01, qty_step=1.0):
  ts_ns,kind,price,qty,bid_price,bid_qty,ask_price,ask_qty,is_buyer_maker
  - "B" rows: top-of-book (level-1), prices in DOLLARS so to_ticks(price/0.01) => integer cents.
  - "T" rows: executions; is_buyer_maker = 1 iff a BUY limit order was executed (buyer was the maker).
  - ts_ns = round(Time * 1e9). Only ordering/clock-sampling matter in the backtest.

Usage:
  python3 scripts/lobster_to_tape.py <message.csv> <orderbook.csv> <out.tape>
"""
import csv
import os
import sys

# LOBSTER empty-level price sentinels are +-9999999999.
_SENTINEL = 9_999_999_990


def _dollars(x: int):
    return None if abs(x) >= _SENTINEL else f"{x / 1e4:.4f}"


def main() -> int:
    if len(sys.argv) != 4:
        sys.stderr.write("usage: lobster_to_tape.py <message.csv> <orderbook.csv> <out.tape>\n")
        return 2
    msg_path, ob_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    n = n_b = n_t = 0
    last = (None, None, None, None)  # bp, bq, ap, aq

    with open(msg_path) as mf, open(ob_path) as of, open(out_path, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["ts_ns", "kind", "price", "qty",
                    "bid_price", "bid_qty", "ask_price", "ask_qty", "is_buyer_maker"])

        for mline, oline in zip(mf, of):
            m = mline.rstrip("\n").split(",")
            o = oline.rstrip("\n").split(",")
            if len(m) < 6 or len(o) < 4:
                continue

            t = float(m[0]); typ = int(m[1]); size = int(m[3]); price = int(m[4]); direction = int(m[5])
            ts_ns = int(round(t * 1e9))
            n += 1

            # Execution -> trade row first (visible=4, hidden=5).
            if typ in (4, 5) and price > 0:
                ibm = "1" if direction == 1 else "0"   # buy LO executed => buyer was the maker
                w.writerow([ts_ns, "T", f"{price / 1e4:.4f}", size, "", "", "", "", ibm])
                n_t += 1

            # Top-of-book from level-1: AskP1,AskS1,BidP1,BidS1.
            ask_p = _dollars(int(o[0])); ask_s = int(o[1])
            bid_p = _dollars(int(o[2])); bid_s = int(o[3])
            bp = bid_p if bid_p is not None else ""
            ap = ask_p if ask_p is not None else ""
            bq = str(bid_s) if bp != "" else ""
            aq = str(ask_s) if ap != "" else ""

            if (bp, bq, ap, aq) != last:
                w.writerow([ts_ns, "B", "", "", bp, bq, ap, aq, ""])
                last = (bp, bq, ap, aq)
                n_b += 1

    sys.stderr.write(
        f"[lobster_to_tape] {n:,} messages -> {n_b:,} book rows + {n_t:,} trades -> {out_path}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
