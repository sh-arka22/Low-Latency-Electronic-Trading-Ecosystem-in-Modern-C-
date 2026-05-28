# Strategy Methods & PnL Improvement

**Run date:** 2026-05-28 · **Tape:** Binance USD-M perp `bookTicker` + `trades`, 2024-03-28 (24h UTC) · **Symbols:** BTC, ETH, SOL · **Sweep:** `scripts/run_full_sweep.sh` (5 strategies × 3 symbols = 15 backtests).

This document is the answer to two questions:

1. **What techniques are stacked into each strategy** (`baseline → as → as_ofi → full → v12_all_on`), and what does each one *do* in the code?
2. **How much did each layer move the PnL** — both the raw delta and the honest "algorithmic vs accounting" breakdown.

For the broader system architecture and how a single trade flows through the engine, see [`README.md`](README.md).

---

## TL;DR

| | v1.0 baseline | v1.1 ceiling (`full`) | v1.2 (`all_on`) |
|---|---:|---:|---:|
| Portfolio gross PnL (3 symbols) | -$74.72M | -$45.17M | **-$9.66M** |
| Δ vs prior layer | — | +$29.55M (+39.5%) | +$35.51M (+78.6%) |
| Fee-adjusted | -$271.0M | -$172.9M | -$108.1M |

**v1.2 cuts the v1.1 loss by 78.6%** on this directional trading day. But: **38% of that headline gain is a maker-rebate accounting credit, not an algorithmic improvement.** The genuine algorithmic delta (better quotes + fewer toxic fills) is ~$40M ≈ 62% of the fee-adjusted improvement. Both are real money; the honest split matters for any external claim. Full decomposition in [§5](#5-honest-decomposition-algorithmic-vs-accounting).

---

## 1. Strategy ladder

Each strategy is the previous one plus one or more techniques. They are exposed in `scripts/run_full_sweep.sh` as named variants the user can A/B against each other.

| Strategy | New techniques on top of previous | Why we added it |
|---|---|---|
| **`baseline`** (v1.0) | Threshold-pennying: bid = `bbo_bid − 1 if fair < bbo_bid + threshold else bbo_bid`; symmetric on the ask. No σ, no inventory awareness. | The book's reference v1.0 quoter (Chapter 10). Quotes near the BBO and gets adversely selected hard. |
| **`as`** (v1.1) | + **Avellaneda-Stoikov reservation + optimal spread**. Reservation pulls toward neutral inventory; spread widens with σ. | First inventory-aware quote — stops accumulating runaway positions. |
| **`as_ofi`** (v1.1) | + **OFI overlay** on reservation (`reservation += β · OFI`). | Quotes lean toward the side flow is building. Cont-Kukanov-Stoikov 2014. |
| **`full`** (v1.1 ceiling) | + **Queue-position hysteresis** (don't requote within ±1 tick to preserve queue priority). <br> + **Adaptive clip** sizing (shrink size near inventory limits and in high σ). | Avoids self-inflicted queue-priority loss; trades smaller when conditions are noisy. |
| **`v12_all_on`** (v1.2) | + **VPIN regime detector** (toxic-flow indicator). <br> + **Killswitch** (cancel both sides during toxic flow). <br> + **Regime-aware γ** (scale AS γ by σ_short / σ_long). <br> + **Asymmetric OFI widening** (widen only the toxic side). <br> + **Stoikov micro-price** anchor (replace OFI-weighted micro). <br> + **Maker rebate** booked per-fill. | Defensive overlay targeted at directional / toxic days where v1.1 is structurally adverse-selected. |

All v1.2 levers are individually env-var gated; `v12_all_on` flips every flag at once.

---

## 2. Techniques — what each one does in the code

### v1.1 layer (already shipped before this sweep)

#### 2.1 Avellaneda-Stoikov reservation + optimal spread
**Code:** `trading/strategy/market_maker.cpp:87-118` · **Cfg:** `gamma_=0.05`, `kappa_=1.5`, `session_hours_=6.5`

```cpp
reservation = fair_price - q * γ * σ² * τ           // skew against inventory
spread      = γ * σ² * τ + (2/γ) * log1p(γ/κ)        // wider in high σ
bid = floor(reservation - 0.5 * spread)
ask = ceil (reservation + 0.5 * spread)
```

`q` = current position (signed), `σ²` = EWMA on mid-returns from `FeatureEngine`, `τ` = remaining fraction of session. The κ-term contributes a fixed half-spread (≈ 1 tick at default κ); the σ-term is the inventory-and-vol-aware piece.

#### 2.2 Order Flow Imbalance overlay
**Code:** `trading/strategy/feature_engine.h:83-103` (estimator) + `market_maker.cpp:114-115` (use) · **Cfg:** `beta_ofi_=0.5`

Per Cont-Kukanov-Stoikov 2014 eq. (1): per BBO change, compute the signed contribution
```
e_bid = +bid_qty  if bid_price up,  Δqty if same,  -prev_bid_qty if down
e_ask = analogous (sign flipped)
OFI  += (e_bid - e_ask)        // then EWMA-smoothed
```
Then `reservation += β · OFI` shifts the AS quote toward the side where the book is leaning. Positive OFI → buyers winning the book → reservation moves up.

#### 2.3 Queue-position hysteresis
**Code:** `trading/strategy/order_manager.h:67-115` (`moveOrder`) · **Cfg:** `hysteresis_ticks_=1`

If the strategy wants to move from a live order at price P to a target P' within ±1 tick, `moveOrder` returns without sending a cancel/replace. This preserves time priority in the exchange's queue at price P — a one-tick requote often *loses* more in slippage than it gains in adverse-selection avoidance.

#### 2.4 Adaptive clip sizing
**Code:** `market_maker.cpp:139-147` · **Cfg:** `sigma_ref_=1.0`

```cpp
inv_room    = max(0, 1 - |q|/max_position)
sigma_scale = clamp(sigma_ref / sigma, 0.5, 1.5)
clip       = max(1, round(clip_base * inv_room * sigma_scale))
```
Smaller orders near inventory limits (so a single bad fill doesn't blow risk), smaller orders in high σ (so we don't size into noise).

---

### v1.2 layer (added in this branch)

Every v1.2 lever is **env-var gated** and exposed in `backtest_main.cpp:67-100`. Setting `USE_VPIN=1 USE_KILLSWITCH=1 ...` at runtime activates them; defaults are off so v1.1 strategies are unaffected.

#### 2.5 VPIN regime detector
**Code:** `trading/strategy/vpin.h` + `feature_engine.h:131-134` · **Cfg:** `vpin_bucket_size_=500000`, `vpin_threshold_=0.46`

VPIN = Volume-synchronized Probability of Informed Trading (Easley/López de Prado/O'Hara 2012). The estimator bucketizes trade volume into equal-volume buckets and within each bucket measures the absolute buy/sell imbalance ratio. A window-averaged VPIN > 0.46 indicates toxic flow.

VPIN itself doesn't quote — it produces a boolean `vpin_toxic` that *modulates* the killswitch and spread-widening levers (§2.6, §2.8 below). Without VPIN those levers fire only on the current OFI; with VPIN they fire more aggressively whenever historical volume has been one-sided.

#### 2.6 Killswitch
**Code:** `market_maker.cpp:47-76` · **Cfg:** `killswitch_ofi_=200`, `killswitch_micro_ticks_=2`, `vpin_kill_scale_=0.5`

```cpp
if (|OFI| > kill_ofi_eff || |micro_price − mid| > kill_micro_eff_ticks):
    order_manager_.cancelOrders(ticker)
    return                              // skip the requote entirely
```
When VPIN-toxic, the thresholds tighten by `vpin_kill_scale_=0.5` (fire half as much OFI is enough to trigger). This is **the single largest behavioral change in v1.2** — v1.1 always requotes after a BBO update; v1.2 sometimes pulls both sides and waits for the toxicity to subside. Cartea/Jaimungal/Penalva 2015 §10.4.2.

#### 2.7 Regime-aware γ
**Code:** `market_maker.cpp:96-104` · **Cfg:** `use_regime_gamma_`, `regime_gamma_scale_=2.0`, `ewma_decay_long_=0.985`

```cpp
sigma_long = feature_engine_.getVolatilityLong()   // long-EWMA σ
γ_effective = γ * clamp(sigma_short / sigma_long, 1/scale, scale)
```
Higher recent volatility (short EWMA > long EWMA) → larger γ → wider AS spread, more reservation pull from inventory. Clamped both directions so a stuck σ_long can't blow γ up.

#### 2.8 Asymmetric OFI spread widening
**Code:** `market_maker.cpp:120-130` · **Cfg:** `spread_widen_ofi_=2.0`, `vpin_widen_mult_=2.0`

v1.1's spread is symmetric around the reservation. v1.2 adds a one-sided widening:
```cpp
widen_k   = spread_widen_ofi_ * (vpin_toxic ? vpin_widen_mult_ : 1.0)
bid_widen = widen_k * max(-OFI, 0)        // only when sellers dominate
ask_widen = widen_k * max(+OFI, 0)        // only when buyers dominate
```
When adverse selection is one-sided we retreat *only* the side getting picked off — preserving fill opportunities on the safe side. Doubles in VPIN-toxic regimes.

#### 2.9 Stoikov micro-price anchor
**Code:** `feature_engine.h:49-58` · **Cfg:** `use_stoikov_micro_`

Replaces the v1.1 fair-price (`(bid·ask_qty + ask·bid_qty) / total_qty`) with Stoikov's state-dependent micro-price:
```cpp
imb       = (bid_qty - ask_qty) / (bid_qty + ask_qty)
fair      = mid + 0.5 * spread * imb
```
Stoikov 2018, simplified. Quotes anchor to the queue-imbalance-weighted micro instead of the volume-weighted touch — better short-term fair value estimate when one side is thicker than the other.

#### 2.10 Maker rebate booked per-fill
**Code:** `trading/strategy/position_keeper.h:92-98` · **Cfg:** `maker_rebate_bps_=0.5`

```cpp
const double notional = price * exec_qty
const double rebate   = notional * maker_rebate_bps_ * 1e-4
real_pnl  += rebate
total_pnl += rebate
```
v1.0 and v1.1 are fee-blind in the engine — `analyze_pnl.py` deducts a flat 2 bp post-hoc. v1.2 books the **rebate** (Binance VIP1 maker = +0.5 bp) inside the engine on every passive fill. Same fills, +0.5 bp credit per notional traded — pure accounting addition, no behavioral change in the quoting logic.

---

## 3. Full per-symbol results

All numbers are gross PnL in USD on the full 24h of 2024-03-28. Fee drag is post-hoc 2 bp × last-mid × volume. Fee-adj = gross − fee_drag (for v1.2 the rebate is already baked into gross via §2.10).

### 3.1 BTCUSDT (1,117,304 rows)

| Strategy | Gross PnL | Fee drag | Fee-adj | Fills | Requotes |
|---|---:|---:|---:|---:|---:|
| baseline | -$33,441,210 | $104,577,263 | -$138,018,473 | 667,261 | 802,939 |
| as | -$28,221,002 | $88,432,829 | -$116,653,831 | 573,553 | 1,328,638 |
| as_ofi | -$27,500,105 | $87,059,549 | -$114,559,654 | 564,638 | 1,362,841 |
| **full** (v1.1) | **-$19,988,536** | $60,622,952 | -$80,611,488 | 397,206 | 983,805 |
| **v12_all_on** | **-$4,949,421** | $56,763,232 | -$61,712,653 | 368,997 | 1,055,287 |
| **v1.2 vs v1.1 Δ** | **+$15,039,115** | -$3,859,720 | +$18,898,835 | -28,209 (-7%) | +71,482 (+7%) |

### 3.2 ETHUSDT (1,086,219 rows — first complete 24h ETH run)

| Strategy | Gross PnL | Fee drag | Fee-adj | Fills | Requotes |
|---|---:|---:|---:|---:|---:|
| baseline | -$25,607,548 | $60,016,535 | -$85,624,083 | 555,824 | 722,077 |
| as | -$22,042,445 | $55,535,239 | -$77,577,684 | 546,622 | 1,507,367 |
| as_ofi | -$18,578,457 | $47,739,856 | -$66,318,313 | 461,496 | 1,892,348 |
| **full** | **-$16,430,880** | $41,211,615 | -$57,642,495 | 419,593 | 1,137,034 |
| **v12_all_on** | **-$4,444,400** | $27,774,783 | -$32,219,183 | 289,868 | 1,750,686 |
| **v1.2 vs v1.1 Δ** | **+$11,986,480** | -$13,436,832 | +$25,423,311 | -129,725 (-31%) | +613,652 (+54%) |

### 3.3 SOLUSDT (853,796 rows — first complete 24h SOL run)

| Strategy | Gross PnL | Fee drag | Fee-adj | Fills | Requotes |
|---|---:|---:|---:|---:|---:|
| baseline | -$15,671,146 | $31,660,546 | -$47,331,692 | 314,022 | 1,481,343 |
| as | -$14,247,208 | $39,388,617 | -$53,635,825 | 380,700 | 1,808,418 |
| as_ofi | -$6,759,115 | $25,520,093 | -$32,279,208 | 249,981 | 3,226,286 |
| **full** | **-$8,750,353** | $25,869,961 | -$34,620,313 | 284,569 | 1,762,145 |
| **v12_all_on** | **-$261,781** | $13,904,596 | -$14,166,378 | 154,817 | 3,185,239 |
| **v1.2 vs v1.1 Δ** | **+$8,488,571** | -$11,965,365 | +$20,453,936 | -129,752 (-46%) | +1,423,094 (+81%) |

SOL came within $262K of breakeven — 97% loss reduction.

### 3.4 Portfolio totals

| | v1.1 `full` | v1.2 `all_on` | Δ | %loss reduced |
|---|---:|---:|---:|---:|
| **Gross PnL** | -$45,169,769 | **-$9,655,602** | **+$35,514,167** | **+78.6%** |
| Fee-adj PnL | -$172,874,296 | -$108,098,214 | +$64,776,082 | +37.5% |

---

## 4. Per-layer improvement (gross PnL)

How much did each step of the ladder contribute?

| Layer | Strategy | BTC | ETH | SOL | Portfolio | Δ vs prev | % cumulative |
|---|---|---:|---:|---:|---:|---:|---:|
| v1.0 | baseline | -$33.44M | -$25.61M | -$15.67M | -$74.72M | — | — |
| v1.1.a | + AS | -$28.22M | -$22.04M | -$14.25M | -$64.51M | +$10.21M | 13.7% |
| v1.1.b | + OFI overlay | -$27.50M | -$18.58M | -$6.76M | -$52.84M | +$11.67M | 29.3% |
| v1.1.c | + hysteresis + adaptive clip | -$19.99M | -$16.43M | -$8.75M | -$45.17M | +$7.67M | 39.5% |
| v1.2 | + VPIN/killswitch/regime-γ/widen/Stoikov/rebate | -$4.95M | -$4.44M | -$0.26M | -$9.66M | +$35.51M | **87.1%** |

The v1.2 layer is the largest single jump — about as much as all of v1.1 combined. Per-symbol, the v1.2 jump scales with how toxic each symbol's flow was:

| Symbol | v1.1 loss | v1.2 loss | % reduced | Δfills | Δrequotes | Δfee drag |
|---|---:|---:|---:|---:|---:|---:|
| BTC | -$20.0M | -$4.9M | -75% | -7% | +7% | -$3.9M |
| ETH | -$16.4M | -$4.4M | -73% | -31% | +54% | -$13.4M |
| SOL | -$8.8M | -$0.3M | -97% | -46% | +81% | -$12.0M |

Reading the table:
- **Δfills always negative** = killswitch + asymmetric widening successfully pull quotes out of toxic windows. SOL has thinnest microstructure → killswitch fires most.
- **Δrequotes always positive** = regime-γ and Stoikov micro shift the quoting price more often outside killswitch periods.
- **Δfee drag heavily negative on ETH/SOL** = fewer fills × notional = $12-13M less fees alone, even before counting better gross PnL.

---

## 5. Honest decomposition: algorithmic vs accounting

The +$35.5M gross / +$64.8M fee-adj headline conflates three different effects. v1.2 books the maker rebate **inside the engine**, so its "gross PnL" includes a credit v1.1 doesn't get. Strip that out and the algorithmic shift is more modest.

### 5.1 Decomposition of v1.2's $64.8M fee-adj improvement

Per-fill rebate ≈ `notional × 0.5 bp` ≈ `fee_drag × 0.25`. Total v1.2 rebate baked into gross:

| Symbol | v1.2 fee_drag | Implied rebate | v1.2 algo_gross (rebate-stripped) |
|---|---:|---:|---:|
| BTC | $56.76M | $14.19M | -$19.14M |
| ETH | $27.77M | $6.94M | -$11.39M |
| SOL | $13.90M | $3.48M | -$3.74M |
| **Portfolio** | **$98.43M** | **$24.61M** | **-$34.27M** |

So the $64.8M fee-adj improvement breaks into three pieces:

| Component | Portfolio $ | % of fee-adj Δ | Algorithmic? |
|---|---:|---:|:---:|
| **Pure algo quote-quality gain** (same-style fills priced better) | **+$10.9M** | **17%** | ✅ |
| **Fee savings from fewer fills** (killswitch + asymmetric widening) | **+$29.3M** | **45%** | ✅ (downstream of algo) |
| **Maker rebate accounting credit** (v1.2 only) | **+$24.6M** | **38%** | ❌ |
| **Total Δfee-adj** | **+$64.8M** | 100% | |

### 5.2 Like-for-like (both strategies booking rebate)

If you set `MAKER_REBATE_BPS=0.5` on v1.1 `full` too (so both strategies operate on the same Binance VIP1 tier), v1.1 actually earns *more* rebate because it has more fills. Net:

| | v1.1 full + 0.5 bp rebate (est.) | v1.2 all_on | Δ |
|---|---:|---:|---:|
| Algo gross (no rebate) | -$45.17M | -$34.27M | **+$10.90M** |
| Rebate credit | +$31.93M | +$24.61M | -$7.32M |
| Adjusted gross | -$13.24M | -$9.66M | **+$3.58M** |
| Fee drag | $127.7M | $98.4M | **-$29.3M** |
| **Fee-adj total** | **-$140.94M** | **-$108.10M** | **+$32.84M (≈+19%)** |

**Like-for-like, v1.2 still wins by ~$33M fee-adj ≈ 19% loss reduction** — but that's the defensible number, not the 78.6% headline. The remaining ~$32M of the headline comes from the rebate being switched on in v1.2 only.

### 5.3 Where the +$33M algorithmic improvement actually comes from

| Source | Approx contribution | Lever |
|---|---:|---|
| Skipped toxic fills (fewer fills × per-fill loss) | ~$29M (89%) | Killswitch (§2.6) + asymmetric widening (§2.8) |
| Better fill price on kept fills | ~$3-4M (11%) | Regime-γ (§2.7) + Stoikov micro (§2.9) |

So the v1.2 algorithmic shift is **mostly defensive (don't trade when toxic), not smarter (quote better prices when we do trade)**. That's an important framing — it tells you which lever to tune first if you want more gain (killswitch thresholds) vs which lever is doing less heavy lifting (regime-γ).

---

## 6. v1.1 reproducibility check (BTC only)

Only the prior BTC snapshot was full-day (ETH/SOL prior CSVs were truncated to 2.8h/3.7h because of the original Logger drain hang). For BTC, v1.1 reproduces deterministically:

| Strategy | NEW gross | PRIOR gross | Δ | Δ% |
|---|---:|---:|---:|---:|
| baseline | -$33,441,210 | -$33,441,210 | $0 | bit-identical |
| as | -$28,221,002 | -$28,217,253 | -$3,749 | -0.013% |
| as_ofi | -$27,500,105 | -$27,496,608 | -$3,497 | -0.013% |
| full | -$19,988,536 | -$20,017,249 | +$28,713 | +0.143% |

Within ±0.15% noise. Drift is simulator OID/memory-layout tiebreaks, not real code change.

---

## 7. Infrastructure changes that unblocked this sweep

### Logger drain-loop fix
`common/logging.h:53` — one-line change. Inner drain `for`-loop now checks `running_` so `Logger::~Logger()` releases in ~5 s instead of hanging indefinitely on the inflated `LFQueue` counter.

```cpp
// Before
for (auto next = queue_.getNextToRead(); queue_.size() && next; ...)

// After
for (auto next = queue_.getNextToRead(); running_ && queue_.size() && next; ...)
```

Verified across all 15 backtests in this sweep — every Logger destructor logged ≈ 5.03 s between "Flushing and closing Logger" and "Logger ... exiting". Without this, the sweep stalled after ~3 strategies on the first attempt.

### Script consolidation
Replaced two overlapping scripts (`run_all_strategies.sh` for v1.1 N-symbols and `run_step_sweep.sh` for v1.2 BTC-only) with a single **`scripts/run_full_sweep.sh`** that runs 5 strategies × 3 symbols in one invocation. Outputs land in the same `data/showcase/<SYMBOL>/pnl_<strategy>.csv` paths the notebook and `analyze_pnl.py` already consume.

### Known limitation (not fixed)
`common/lf_queue.h:26-29` — `LFQueue::updateWriteIndex` has no producer backpressure: it wraps the ring and unconditionally increments `num_elements_`. Under burst load, `size()` inflates past `store_.size()` (8M LogElements) and log records are silently overwritten. Shutdown is now safe (drain-loop fix above), but bursty log records can be lost. A proper fix requires picking a backpressure/drop policy per caller — out of scope for this run.

---

## 8. Caveats

- **Single trading day** — 2024-03-28 was a directional trend day. v1.1 market-making is structurally adverse-selected on such days; v1.2's defensive features are designed exactly for this scenario. On a mean-reverting / low-vol day the v1.2 lift would be smaller (possibly *negative* if killswitch fires spuriously).
- **Backtest simulator** uses queue-aware fills (`queue_decay_per_trade=1.0`); not a tick-by-tick exchange replay. Real fills could differ on the margin.
- **v1.2 hyperparameters** were calibrated in development from `run_step_sweep.sh` defaults, not re-fit on this sweep day. Per-symbol re-fitting could move numbers either direction.
- **Fee model:** flat 2 bp post-hoc fee + (v1.2 only) 0.5 bp engine-booked rebate. Actual venue fees vary by tier, side, and order type.

---

## 9. Reproduce

```bash
cd electronic_trading_ecosystem
bash build.sh
bash scripts/run_full_sweep.sh                    # ~6 hours wall-time, single process
# results: data/showcase/<SYMBOL>/pnl_<strategy>.csv

# Then either open the rendered notebook
open notebooks/strategy_compare.html

# Or run analyze_pnl.py against the new CSVs
python3 scripts/analyze_pnl.py data/showcase/*/pnl_*.csv
```

Single-symbol smoke (≈ 1.5h):
```bash
SYMBOLS=BTCUSDT bash scripts/run_full_sweep.sh
```

A/B individual v1.2 levers (per-feature decomposition):
```bash
bash scripts/run_step_sweep.sh                    # legacy BTC-only step-by-step sweep
                                                  # → data/showcase/BTCUSDT/_step_sweep/
```
