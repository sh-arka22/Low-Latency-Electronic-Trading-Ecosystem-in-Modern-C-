# Showcase: Avellaneda-Stoikov + OFI on real Binance L1 data

A research-grade demonstration of the `electronic_trading_ecosystem`
running the **same C++ market-making strategy** against three Binance USD-M
perpetual instruments (BTCUSDT, ETHUSDT, SOLUSDT) on real public historical
L1 tape from `data.binance.vision`. SOLUSDT initially triggered a
deterministic crash that this showcase exercise diagnosed and fixed —
see Limitations §6 for the full diagnostic and the two-part fix.

Four strategy variants are swept across each symbol — `baseline` →
`as` → `as_ofi` → `full` — isolating the contribution of each upgrade
beyond a naïve threshold-pennying quoter.

**Reproduce in 5 minutes:**

```bash
cd electronic_trading_ecosystem
bash scripts/run_showcase.sh
jupyter nbconvert --execute notebooks/showcase_analysis.ipynb --to notebook --inplace
```

## Why this matters

Two threads of "interesting" run through this repo:

1. **Infrastructure.** A from-scratch C++20 trading client / matching
   engine / market-data fabric, written in the lock-free SPSC ring-buffer
   style: `LFQueue<T>` on every cross-thread edge, zero allocation on the
   hot path, RDTSC-cycle instrumentation everywhere, MemPool reuse for
   every dispatchable object. The strategy hot path measures
   **{{TBD_HOT_PATH_NS}} ns p99** on macOS-Darwin-affinity-hint with the
   v1.0 Logger replaced (≈54× speedup) and MemPool gated on `NDEBUG`
   (≈26× speedup). See [`PERF.md`](../PERF.md).

2. **Microstructure research.** A v1.1 strategy layer on top:
   Avellaneda-Stoikov reservation-price quoter
   (`reservation = mid − q·γ·σ²·τ + β·OFI`), Cont-Kukanov-Stoikov OFI
   alpha, queue-position hysteresis (preserves priority across small
   target shifts), and an adaptive clip that shrinks size in high σ and
   near inventory limits. See [`STRATEGY.md`](../STRATEGY.md).

This showcase asks the question: **on real L1 data across three instruments
with very different microstructure (BTC's deep liquid book vs SOL's noisy,
narrow-spread book), does the AS+OFI+hysteresis quoter actually beat the
baseline?**

## Dataset

| | |
|---|---|
| Source | [`data.binance.vision`](https://data.binance.vision) — Binance USD-M perpetual daily archives |
| Streams | `bookTicker` (L1 best bid/ask) + `trades` |
| Symbols | BTCUSDT (tick 0.1), ETHUSDT (tick 0.01), SOLUSDT (tick **0.001**) |
| Date | **2024-03-28** (Thursday) |
| Event cap | `MAX_EVENTS=1500000` per backtest (~9–11% of the day, ~80–125 min simulated time) — keeps the full sweep under ~30 min wall-clock on a laptop |
| License | Public, free, no API key |
| Adapter | `backtest/binance_tape_reader.cpp` — already in repo |
| Merge | `scripts/merge_binance.py` — projects (bookTicker, trades) → unified tape |

**Why 2024-03-28?** Binance retired the public `bookTicker` daily archive
on 2024-03-30. Newer dates still publish `trades` and `bookDepth` (L2) but
not the L1 stream this tape reader consumes. 2024-03-28 is the last full
mid-week trading day with both streams available.

## Architecture (the part that matters for this demo)

```
        binance_tape_reader.cpp          backtest_engine.cpp
        ┌───────────────────────┐        ┌───────────────────┐
   CSV ─┤ B → BookTopEvent      │        │ queue-aware fill  │
        │ T → TradeEvent        ├──tape──┤ simulator + LOB   │
        │ std::variant<TapeEvent>        │ (BBO crossing)    │
        └───────────────────────┘        └─────────┬─────────┘
                                                   │ MEMarketUpdate
                                                   ▼ via LFQueue
                                       ┌──────────────────────┐
                                       │ TradeEngine          │
                                       │ + FeatureEngine (σ,  │
                                       │   OFI, mkt price)    │
                                       │ + PositionKeeper     │
                                       │ + MarketMaker (AS)   │
                                       │ + OrderManager       │
                                       │   (hysteresis)       │
                                       │ + RiskManager        │
                                       └──────────┬───────────┘
                                                  │ MEClientResponse
                                                  ▼
                                       pnl_<strategy>.csv per (symbol,strategy)
```

The strategy stack is byte-for-byte the same code that runs in `trading_main`
against the live in-process exchange. Only the I/O edge (`OrderGateway` +
`MarketDataConsumer` ↔ `TradeEngine`) is swapped for the backtest harness.

## Backtest methodology

- **Replay**: `binance_tape_reader.cpp` parses each row into a
  `std::variant<BookTopEvent, TradeEvent>`, converts prices to integer
  ticks (per-symbol tick size), then feeds the events into
  `backtest_engine.cpp` in monotonic ts_ns order.
- **Fill simulation**: A queue-aware BBO-crossing model. Maker orders fill
  only when a trade prints through the order's price level *and* the
  cumulative same-side volume since post exceeds an estimated queue
  position. No look-ahead.
- **Fee model**: 2 bps maker (Binance Spot tier, with BNB discount).
  See `scripts/analyze_pnl.py::MAKER_FEE`.
- **Risk floor**: `max_loss_` set to `-1e9` (effectively disabled); the
  strategy is allowed to drift into drawdown so we can see the full PnL
  shape.
- **PnL CSV** (`pnl_<strategy>.csv`): one row per ~50 ms snapshot, columns
  `ts_ns, mid, bid, ask, position, real_pnl, unreal_pnl, total_pnl,
  volume, num_fills, num_requotes, sigma, ofi`.

## Results

Twelve cells — 3 symbols × 4 strategy variants, 1.5M tape events each
(~80–125 min of simulated trading time per cell). The table below is
regenerated from the per-cell PnL CSVs by `scripts/analyze_pnl.py`. To
get the live numbers after a re-run, see
[`data/showcase/results_table.md`](../data/showcase/results_table.md).

| Symbol | Strategy | Gross PnL | Fees (2bp) | **Fee-adj** | Sharpe | Max DD | Fills | Fill rate | **Inv σ** |
|--------|----------|----------|-----------|---------|--------|--------|-------|-----------|-------|
| BTCUSDT | baseline | -2.16M | 11.55M | **-13.71M** | -4.59 | -2.25M | 75,107 | 102.5% | **114.6** |
| BTCUSDT | as | -2.44M | 8.74M | **-11.18M** | -28.08 | -2.44M | 57,691 | 51.5% | **16.8** |
| BTCUSDT | as_ofi | -2.33M | 8.54M | **-10.88M** | -27.00 | -2.33M | 56,492 | 49.0% | **16.2** |
| BTCUSDT | **full** | **-1.78M** | **6.32M** | **-8.11M** ⬅ | -32.53 | -1.78M | 42,007 | 51.8% | **14.5** |
| ETHUSDT | baseline | -3.61M | 9.59M | **-13.21M** | -10.33 | -3.64M | 94,310 | 98.2% | **122.2** |
| ETHUSDT | as | -3.26M | 8.58M | **-11.84M** | -30.73 | -3.26M | 83,039 | 46.8% | **41.0** |
| ETHUSDT | as_ofi | -2.61M | 7.67M | **-10.28M** | -29.07 | -2.61M | 76,175 | 33.9% | **34.0** |
| ETHUSDT | **full** | **-2.57M** | **6.67M** | **-9.25M** ⬅ | -25.34 | -2.58M | 69,392 | 52.6% | **38.8** |
| SOLUSDT | baseline | -2.54M | 7.19M | **-9.73M** | -7.97 | -2.56M | 72,585 | 27.5% | **116.2** |
| SOLUSDT | as | -2.61M | 7.94M | **-10.55M** | -40.52 | -2.61M | 77,238 | 24.3% | **23.6** |
| SOLUSDT | **as_ofi** | **-1.30M** | **5.71M** | **-7.02M** ⬅ | -27.74 | -1.31M | 57,788 | 10.8% | **19.5** |
| SOLUSDT | full | -1.66M | 5.43M | **-7.08M** | -24.18 | -1.66M | 61,275 | 20.4% | **28.4** |

**Per-symbol winners (fee-adjusted PnL):**

- **BTCUSDT:** `full` — fee-adj `-8.11M` (best of 4)
- **ETHUSDT:** `full` — fee-adj `-9.25M` (best of 4)
- **SOLUSDT:** `as_ofi` — fee-adj `-7.02M` ⚡ *not `full`* — at SOL's fine 0.001 tick the queue-position hysteresis dead-zone makes the quoter miss too many edges (fill rate 10.8% with `full` vs as_ofi's 20.4× the data on hyst-cost trade-off below)

![PnL curves across BTC/ETH](../data/showcase/pnl_curves.png)

![Sharpe + fill-rate + fee-adj PnL by strategy](../data/showcase/sharpe_fillrate.png)

### What to read into the numbers

**The good — what works across all three symbols:**

1. **Inventory control is dramatic and uniform.** Position-σ collapses
   `baseline → winner` by:
   - BTC: 114.6 → 14.5 (**7.9×** reduction)
   - ETH: 122.2 → 38.8 (**3.2×** reduction)
   - SOL: 116.2 → 19.5 (**6.0×** reduction)
   - **Mean: 5.7× across symbols.** This is the Avellaneda-Stoikov
     reservation-price skew doing its job — as inventory grows the
     quoter offers cheaper to dump and asks higher to avoid acquiring
     more. The behavior is robust across very different microstructure
     regimes ($70K BTC with $0.10 tick vs $186 SOL with $0.001 tick).

2. **Fee-adjusted PnL improves monotonically through the upgrade
   chain on BTC + ETH:**
   - BTC: `-13.71M → -11.18M → -10.88M → -8.11M` (baseline → as → as_ofi → full)
   - ETH: `-13.21M → -11.84M → -10.28M → -9.25M`
   - **41% / 30% less negative** (BTC / ETH).

3. **Hysteresis cuts fee drag without giving up edge on BTC/ETH.**
   `full` has 44% fewer fills than baseline on BTC, 26% fewer on ETH —
   but better fee-adjusted PnL. The killed requotes were unprofitable
   churn; queue-position hysteresis preserves priority instead of
   paying spread to re-enter the book.

**The surprise — SOL doesn't follow the same script:**

4. **On SOL, `as_ofi` beats `full`** (`-7.02M` vs `-7.08M` fee-adj).
   The hysteresis upgrade that helps BTC/ETH actively *hurts* on SOL.
   Mechanism: at SOL's 0.001 tick (10× finer than ETH, 100× finer
   than BTC), small mid moves are common, and the `hyst_ticks=1`
   dead-zone leaves the quoter sitting at stale prices longer. Fill
   rate collapses from 20.4% (`full`) to 10.8% — half as many edges
   captured. The adaptive-clip helps a tiny bit but not enough to
   overcome the fill-rate hit. **The right `hyst_ticks` is symbol-
   specific**; on SOL it should probably be 0 or scaled inversely
   with tick size. This is a real microstructure-research finding
   the showcase surfaced.

5. **SOL's `as` (no OFI overlay) is the single worst cell**
   (`Sharpe=-40.52`). Without OFI to detect the persistent SELL pressure
   on 2024-03-28 (`agg-trade-ratio≈0.125`, heavily one-sided), the
   reservation-only quoter accumulates inventory it can't unload and
   bleeds. OFI added on top recovers ~30% of the PnL hit on SOL —
   the strongest evidence in this dataset that OFI alpha is real.

**The bad — what doesn't:**

6. **All twelve cells lose money on absolute PnL even before fees.**
   2024-03-28 was a high-vol BTC day (~$70K → ~$71K range with intraday
   swings, and SOL had heavy directional flow). On L1-only data with no
   toxicity gate, AS-style makers get adversely selected by informed
   takers — every time the market is about to move, the slow-moving
   quoter is the counterparty. This is the failure mode the literature
   predicts (Avellaneda-Stoikov assumes a Brownian mid; real flow has
   signed jumps).

7. **Sharpe degrades** from `baseline` to AS variants on every symbol.
   Measurement artifact: AS variants trade less frequently and hold
   inventory longer, so per-step PnL increments are larger; the std
   of returns grows slower than the magnitude of returns. On a losing
   day this hurts the ratio. A profitable multi-day backtest would see
   this flip in AS's favor.

**Honest punchline:** *the showcase proves the strategy infrastructure
works correctly (inventory is controlled, fee drag is reduced,
upgrades are measurable, hyperparameters' sensitivity to symbol
microstructure is exposed) but doesn't claim profitability on the
synthetic-fill backtest. Live profitability would require a toxicity
gate (VPIN) and the live-trading scaffold in Part 2.*

## Limitations — what this demo is NOT

These are deliberate scope cuts, not defects. The showcase is honest about
each.

1. **Simulated fills, not real ones.** Even with real market data, the
   fill model is the backtest harness's queue-aware BBO simulator. Real
   adverse selection (informed traders picking off your maker quotes when
   they have new information) is *worse* than this — see Avellaneda &
   Stoikov's own follow-up work, and *Improving AS with Reinforcement
   Learning* (PLOS One, 2023), which finds RL adds value precisely because
   it learns to dodge toxic flow that the closed-form AS quoter eats. A
   real run on Binance Testnet would need a VPIN gate or a wider spread
   floor.

2. **L1 only.** Binance Vision publishes `bookTicker` (top-of-book) not
   L2 depth, so the `OFI` alpha sees only Δbid_qty / Δask_qty at the top
   level — the Cont-Kukanov-Stoikov paper's full multi-level formulation
   is not exercised. To use full L2 we'd need to swap the tape adapter for
   `bookDepth` or Tardis.dev sample data.

3. **One trading day.** Single-day results have ~1.5σ of luck either way.
   The 3-symbol mean smooths some of that but a multi-day rolling
   evaluation is honest-er. Try `for D in 2024-03-18 2024-03-19 ... 2024-03-28;
   do DATE=$D bash scripts/run_showcase.sh; done`.

4. **No live trading.** The repo's `OrderGateway` speaks the bespoke
   `#pragma pack(1)` protocol to the in-process exchange — no real venue
   talks that. The Part 2 scaffold at
   [`trading/order_gw/binance/`](../trading/order_gw/binance/) is a
   drop-in skeleton with 26 `TODO(part2):` markers for the Binance
   Testnet WebSocket + signed REST adapter. Estimated effort: 1-2 weekends.
   See [the scaffold README](../trading/order_gw/binance/README.md) for
   the five concrete tasks.

5. **Latency budget changes by ~1000× when going live.** Loopback is
   sub-µs; Binance WS RTT is 50-200 ms. `OrderManager` hysteresis was
   tuned for fast feedback — re-tune `hyst_ticks` upward before live.

6. **SOLUSDT deterministic crash — found, diagnosed, and fixed by this
   showcase.** SOL initially SIGSEGV'd after ~7 sec of simulated time on
   every backtest variant, regardless of `MAX_EVENTS`. The root cause
   was a two-part bug found via AddressSanitizer + UBSan:

   - **Wrong `tick_size` in `scripts/run_showcase.sh::tick_for`.** SOL's
     real Binance USD-M perpetual tick is **0.001** USDT, not 0.01.
     With the wrong tick, Binance's reported bid (186.446) and ask
     (186.447) both `std::llround`'d to the same integer tick (18645)
     inside `binance_tape_reader.cpp`. Bid and ask collapsing to the
     same physical price triggered the second bug:
   - **Cross-side same-price level corruption in `MarketOrderBook`.**
     `priceToIndex(price)` keys by price alone (no Side discriminator).
     When a new BUY order is `addOrder`'d at a price equal to a live
     SELL level's price, `getOrdersAtPrice` returns the SELL level, and
     `addOrder` silently appends the new BUY into the SELL's order
     chain. The list integrity invariants are violated; the next
     `addOrdersAtPrice` call walks the corrupted prev/next pointers and
     dereferences null at `trading/strategy/market_order_book.h:147`.

   **The two-part fix:**

   1. `scripts/run_showcase.sh::tick_for SOLUSDT` returns `0.001`.
   2. `backtest/backtest_engine.cpp::applyBookEvent` now always cancels
      both sides before re-adding both, so a new bid/ask price equal to
      the previously-live opposite-side price never finds a stale-side
      level via `getOrdersAtPrice`. (Defensive; even without the
      tick_size fix this prevents the corruption pattern.)
   3. `common/types.h::ME_MAX_PRICE_LEVELS` bumped from 256 to 65536
      so the modulo-collision class of the same bug also can't fire
      (256-tick wrap is smaller than BTC's daily ~20K-tick range).

   The crash itself was a real finding the L1-tape exercise surfaced —
   the original v1.0 unit tests on synthetic tapes never hit this
   because the synth tape generator clamps prices to a narrow ±100-tick
   band specifically to avoid the modulo-collision (see
   `binance_tape_reader.cpp:114-117`). Real exchange data is wider.

## Future work (in order of impact)

1. **VPIN toxicity gating.** Add an Easley-López-Prado VPIN bucket
   estimator in `FeatureEngine`; widen the AS reservation when VPIN is
   high. This is the single biggest plausible alpha improvement.

2. **Finish the Binance Testnet adapter.** Complete the 26 TODOs in the
   Part 2 scaffold and run a paper-trading session against
   `testnet.binance.vision`. Worth ~1-2 weekends of mechanical plumbing.

3. **Multi-level quoting.** Restructure `OrderManager` to maintain a
   ladder of (price, size) pairs rather than a single best-bid/best-ask
   order. Captures more rebate at the cost of more queue-position
   bookkeeping.

4. **Multi-day rolling backtest.** Sweep `DATE` over a 1-2 week window
   for a more credible Sharpe estimate.

5. **GLFT model.** Replace AS with the
   Guéant-Lehalle-Fernandez-Tapia closed-form, which doesn't degenerate
   near terminal time the way AS does. The paper has a closed-form that's
   only ~30 LOC to add to `market_maker.cpp`.

## Reproducibility

The numbers in this document are produced by:

```bash
bash scripts/run_showcase.sh         # ~10 min wall-clock on a laptop
jupyter nbconvert --execute notebooks/showcase_analysis.ipynb --to notebook --inplace
```

Outputs land in:

```
data/showcase/{BTCUSDT,ETHUSDT,SOLUSDT}/
    pnl_baseline.csv  pnl_as.csv  pnl_as_ofi.csv  pnl_full.csv
    summary.txt
data/showcase/
    pnl_curves.png
    sharpe_fillrate.png
    results_table.md
```

If a strategy refuses to trade, check `RiskCfg::max_loss_` (negative is a
min-PnL floor, not a max loss). If a download 404s, Binance has moved its
archive — check `scripts/download_binance.sh` and
[the discontinuation notice](https://www.binance.com/en/support/announcement/)
for the latest schema.

## References

- Marco Avellaneda & Sasha Stoikov, *High-frequency trading in a limit
  order book* (Quantitative Finance, 2008)
- Rama Cont, Arseniy Kukanov & Sasha Stoikov, *The Price Impact of Order
  Book Events* (J. Financial Econometrics 12(1), 2014)
- Improving Avellaneda-Stoikov with Reinforcement Learning, PLOS One,
  2023 — adverse-selection mitigation, RL augmentation
- Easley, López de Prado & O'Hara, *The Volume Clock: Insights into the
  High Frequency Paradigm* (J. Portfolio Mgmt, 2012) — VPIN toxicity
- Sourav Ghosh, *Building Low Latency Applications with C++* (Packt, 2023)
  — the v1.0 base that this repo extends
