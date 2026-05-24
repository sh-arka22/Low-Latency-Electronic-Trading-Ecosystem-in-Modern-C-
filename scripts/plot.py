#!/usr/bin/env python3
"""Day 1 — render latency percentile plots from .hgrm files into docs/.

Usage:
    python3 scripts/plot.py latency       # latency_*.hgrm  -> docs/latency.png
    python3 scripts/plot.py pnl            # cmake-build-release/out/pnl_*.csv -> docs/pnl.png
"""
from __future__ import annotations
import csv
import glob
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")  # headless
    import matplotlib.pyplot as plt
except ImportError:
    sys.stderr.write(
        "matplotlib not installed — run: pip install matplotlib pandas\n"
    )
    sys.exit(1)

DOCS_DIR = os.path.join(os.path.dirname(__file__), "..", "docs")
os.makedirs(DOCS_DIR, exist_ok=True)

CPU_GHZ = float(os.environ.get("CPU_GHZ", "2.6"))  # for cycle -> ns


def parse_hgrm(path: str) -> dict:
    """Return {tag, count, p50/p90/p99/p999/p9999, buckets:[(low,high,count)]}."""
    out = {"buckets": []}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("# "):
                k, _, v = line[2:].partition(",")
                out[k.strip()] = v.strip()
                continue
            if line.startswith("bucket_low") or not line:
                continue
            parts = line.split(",")
            if len(parts) != 3:
                continue  # defensive: skip malformed/blank lines
            low, high, c = parts
            try:
                out["buckets"].append((int(low), int(high), int(c)))
            except ValueError:
                continue
    return out


def cmd_latency() -> None:
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # Day 1 filenames are latency_<client_id>_<tag>.hgrm — match either flavour.
    hgrms = sorted(glob.glob(os.path.join(cwd, "latency_*_*.hgrm")))
    if not hgrms:
        hgrms = sorted(glob.glob(os.path.join(cwd, "latency_*.hgrm")))
    if not hgrms:
        hgrms = sorted(glob.glob(os.path.join(cwd, "cmake-build-release", "latency_*.hgrm")))
    if not hgrms:
        sys.stderr.write("no latency_*.hgrm files found\n")
        sys.exit(1)

    rows = []
    for path in hgrms:
        h = parse_hgrm(path)
        if int(h.get("count", "0")) == 0:
            continue
        rows.append({
            "tag":   h.get("tag", os.path.basename(path)),
            "count": int(h["count"]),
            "p50":   int(h["p50"]),
            "p99":   int(h["p99"]),
            "p999":  int(h["p999"]),
            "p9999": int(h["p9999"]),
        })
    if not rows:
        sys.stderr.write("all hgrm files were empty (count=0)\n")
        sys.exit(1)

    rows.sort(key=lambda r: -r["p99"])
    tags = [r["tag"] for r in rows]
    p50  = [r["p50"]   / CPU_GHZ for r in rows]   # cycles -> ns
    p99  = [r["p99"]   / CPU_GHZ for r in rows]
    p999 = [r["p999"]  / CPU_GHZ for r in rows]

    fig, ax = plt.subplots(figsize=(10, max(4, 0.45 * len(rows))))
    y = list(range(len(rows)))
    ax.barh([i - 0.25 for i in y], p50,  height=0.25, label="p50",  color="#4c72b0")
    ax.barh(y,                      p99,  height=0.25, label="p99",  color="#dd8452")
    ax.barh([i + 0.25 for i in y], p999, height=0.25, label="p99.9", color="#c44e52")
    ax.set_yticks(y)
    ax.set_yticklabels(tags, fontsize=8)
    ax.set_xscale("log")
    ax.set_xlabel(f"latency [ns] @ {CPU_GHZ} GHz (log scale)")
    ax.set_title("TradeEngine hot-path latency percentiles")
    ax.legend(loc="lower right")
    ax.invert_yaxis()
    fig.tight_layout()
    out = os.path.join(DOCS_DIR, "latency.png")
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")

    # Also write a tidy CSV next to the PNG for PERF.md to consume.
    csv_out = os.path.join(DOCS_DIR, "latency_summary.csv")
    with open(csv_out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["tag", "count", "p50_ns", "p99_ns", "p999_ns", "p9999_ns"])
        w.writeheader()
        for r in rows:
            w.writerow({
                "tag":      r["tag"],
                "count":    r["count"],
                "p50_ns":   round(r["p50"]   / CPU_GHZ, 1),
                "p99_ns":   round(r["p99"]   / CPU_GHZ, 1),
                "p999_ns":  round(r["p999"]  / CPU_GHZ, 1),
                "p9999_ns": round(r["p9999"] / CPU_GHZ, 1),
            })
    print(f"wrote {csv_out}")


def cmd_pnl() -> None:
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pnl_glob = os.path.join(cwd, "cmake-build-release", "out", "pnl_*.csv")
    files = sorted(glob.glob(pnl_glob))
    if not files:
        sys.stderr.write(f"no PnL CSVs at {pnl_glob}\n")
        sys.exit(1)

    fig, (ax_pnl, ax_pos) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)
    for path in files:
        label = os.path.basename(path).replace("pnl_", "").replace(".csv", "")
        ts, pnl, pos = [], [], []
        with open(path) as f:
            r = csv.DictReader(f)
            for row in r:
                ts.append(int(row["ts_ns"]) / 1e9)
                pnl.append(float(row["total_pnl"]))
                pos.append(int(row["position"]))
        ax_pnl.plot(ts, pnl, label=label, linewidth=1.2)
        ax_pos.plot(ts, pos, label=label, linewidth=1.0, alpha=0.85)
    ax_pnl.set_ylabel("total PnL")
    ax_pnl.set_title("Strategy comparison")
    ax_pnl.legend()
    ax_pnl.grid(alpha=0.3)
    ax_pos.set_ylabel("position")
    ax_pos.set_xlabel("simulated time [s]")
    ax_pos.legend()
    ax_pos.grid(alpha=0.3)
    fig.tight_layout()
    out = os.path.join(DOCS_DIR, "pnl.png")
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def cmd_jitter() -> None:
    """Render pinned vs unpinned rdtsc-loop jitter histograms.

    Looks for docs/jitter_pinned.hgrm and docs/jitter_unpinned.hgrm.
    """
    cwd = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    pinned   = os.path.join(cwd, "docs", "jitter_pinned.hgrm")
    unpinned = os.path.join(cwd, "docs", "jitter_unpinned.hgrm")
    if not (os.path.exists(pinned) and os.path.exists(unpinned)):
        sys.stderr.write(
            "expected docs/jitter_{pinned,unpinned}.hgrm — run "
            "./cmake-build-release/jitter_benchmark first\n")
        sys.exit(1)
    hp = parse_hgrm(pinned)
    hu = parse_hgrm(unpinned)

    fig, ax = plt.subplots(figsize=(10, 5))
    for label, h, color in [
        ("unpinned", hu, "#dd8452"),
        ("pinned (Darwin affinity hint)", hp, "#4c72b0"),
    ]:
        if not h["buckets"]:
            continue
        # Use bucket midpoints (log scale) on x, counts on y, log y
        xs = [(lo + hi) / 2.0 for lo, hi, _ in h["buckets"]]
        ys = [c for _, _, c in h["buckets"]]
        ax.step(xs, ys, where="mid", label=label, color=color, linewidth=1.6)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("rdtsc-to-rdtsc cycle delta (log scale)")
    ax.set_ylabel("samples (log scale)")
    ax.set_title("Tight-loop scheduler jitter — Darwin affinity hint")
    ax.legend()
    ax.grid(alpha=0.3, which="both")

    # Annotate the tail percentiles in the corner.
    annot = (
        f"unpinned  p99={hu['p99']}  p9999={hu['p9999']}  max={hu['max']}\n"
        f"pinned    p99={hp['p99']}  p9999={hp['p9999']}  max={hp['max']}"
    )
    ax.text(0.98, 0.97, annot, transform=ax.transAxes, ha="right", va="top",
            family="monospace", fontsize=9,
            bbox=dict(boxstyle="round", fc="white", ec="0.7", alpha=0.9))

    fig.tight_layout()
    out = os.path.join(DOCS_DIR, "jitter.png")
    fig.savefig(out, dpi=140)
    print(f"wrote {out}")


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in ("latency", "pnl", "jitter"):
        sys.stderr.write(__doc__)
        return 2
    if sys.argv[1] == "latency":
        cmd_latency()
    elif sys.argv[1] == "pnl":
        cmd_pnl()
    else:
        cmd_jitter()
    return 0


if __name__ == "__main__":
    sys.exit(main())
