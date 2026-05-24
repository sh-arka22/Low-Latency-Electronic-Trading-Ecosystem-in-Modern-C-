# Strategy notes — v1.1

## Why this upgrade

The v1.0 `MarketMaker` is a *threshold-pennying* heuristic: it quotes at the best bid and best ask (or 1 tick inside) based on whether the volume-weighted mid leans far enough from the touch. That ignores the three signals a real market maker prices into every quote — **inventory**, **volatility**, and **flow imbalance** — and re-quotes on every book tick, which destroys queue position.

v1.1 replaces the quoting block with **Avellaneda-Stoikov** (inventory + volatility + session clock), adds a **Cont-Kukanov-Stoikov OFI** alpha overlay on the reservation price, and a **queue-position hysteresis** in `OrderManager`. Each piece is opt-in via `TradeEngineCfg` so the v1.0 5-field CLI keeps working.

## The math, mapped to code

### Avellaneda-Stoikov (2008)

For a market maker with inventory `q`, risk-aversion `γ`, mid `s`, instantaneous volatility `σ`, time remaining `τ`, and order-arrival intensity `κ`, the optimal quotes are:

```
reservation_price = s − q·γ·σ²·τ
optimal_spread    = γ·σ²·τ + (2/γ)·ln(1 + γ/κ)
bid = reservation − spread/2
ask = reservation + spread/2
```

Implemented in [`market_maker.cpp:65-92`](trading/strategy/market_maker.cpp) (the block guarded by `cfg.use_as_`). The volatility input comes from a new EWMA estimator in [`feature_engine.h`](trading/strategy/feature_engine.h) (`kEwmaDecay = 0.94`, 50-sample bootstrap). Inventory is read from the existing `PositionKeeper`. Quotes are clamped to the touch so the quoter never crosses its own book.

Source: Avellaneda & Stoikov, "High-frequency trading in a limit order book" (2008).

### Order Flow Imbalance (Cont, Kukanov, Stoikov 2014)

The per-update OFI `e_n` is the signed change in best-bid depth minus the signed change in best-ask depth (CKS eq. 1):

```
        ⎧  Q_b(t)              if  P_b(t) > P_b(t-1)
e_b   = ⎨  Q_b(t) − Q_b(t-1)   if  P_b(t) == P_b(t-1)
        ⎩  −Q_b(t-1)           if  P_b(t) < P_b(t-1)
e_a   = symmetric for ask
e_n   = e_b − e_a
```

An EWMA-smoothed `ofi_` is exposed by `FeatureEngine::getOFI()` and added to the AS reservation as `+ β·OFI`. Positive OFI → buy pressure → shifts quotes higher.

Source: Cont, Kukanov, Stoikov, "The Price Impact of Order Book Events", *Journal of Financial Econometrics* 12(1), 2014.

### Queue-position hysteresis

`OrderManager::moveOrder` used to cancel-and-re-add whenever the strategy's target price differed by even a single tick — every book update destroyed queue position. v1.1 adds a per-ticker `hysteresis_ticks_` dead-zone: the order is kept LIVE while `|target_price − live_price| ≤ hysteresis_ticks_`. Set to 1 in the `full` config, the algo holds its place against single-tick chop.

This is exchange-dependent: most FIFO equity venues reward queue position; pro-rata futures venues less so.

### Adaptive clip

Order size is scaled by inventory headroom and volatility:

```
adaptive_clip = clip · (1 − |q|/q_max) · clamp(σ_ref/σ, 0.5, 1.5)
```

so the algo posts smaller in stressed regimes or when it's near a position limit.

## What we deliberately did not do

These are flagged in `market_maker_enhancements.md` as worth doing but stay out of v1.1 to keep the week shippable and the claims defensible:

- **GLFT (Guéant-Lehalle-Fernandez-Tapia 2013)** — CARA-utility refinement of AS with closed-form hard-inventory boundaries. Worth doing once we have a backtest that reliably distinguishes AS skew quality.
- **VPIN** — volume-bucket toxicity. Needs a fixed-volume-bucket ring buffer; not enough demo signal on synth.
- **Multi-level quoting** — would require restructuring `OMOrderTickerSideHashMap` from `[ticker][side]` to `[ticker][side][level]`.
- **CRTP-replaces-`std::function`** in `TradeEngine::algoOn*_` — useful microbench, low strategy impact.
- **Regime detection (HMM / heuristic switch)** — needs more careful calibration than 1 week allows.

## How to reproduce the comparison

```
cd electronic_trading_ecosystem
bash build.sh
bash scripts/run_all_strategies.sh                 # synthetic tape, 4 strategies
jupyter nbconvert --execute --to html notebooks/strategy_compare.ipynb
open notebooks/strategy_compare.html
```

For a real tape, use `bash scripts/run_all_strategies.sh path/to/binance.csv binance 0.1` — the CSV must already be in the merged `ts_ns,kind,price,qty,bid_price,bid_qty,ask_price,ask_qty,is_buyer_maker` schema (see `backtest/binance_tape_reader.cpp` for the loader). `scripts/download_binance.sh` is provided as a starting point but you'll need to merge `bookTicker-*.csv` and `trades-*.csv` into one timestamped stream first.
