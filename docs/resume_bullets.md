# Resume bullets — `electronic_trading_ecosystem`

Eight drop-in bullets covering the full project: three infra, three research/strategy, one integration, one forward-looking. Pick 3–5 depending on the role:

- **HFT-infra / low-latency C++** → 1, 2, 5, plus 7
- **Quant-researcher / strategy** → 3, 4, 6, plus 8
- **Quant-dev / ML-quant hybrid** → 4, 6, 8, plus 1 or 5
- **ML-engineer applying ML to systems** → 4, 6, 8, plus 7

Numbers are from the canonical full-day sweep (`bash scripts/run_full_sweep.sh`, 24h Binance 2024-03-28 BTC+ETH+SOL × 5 strategies = 15 backtests). See [`RESULTS.md`](RESULTS.md) for the full per-symbol tables and honest decomposition; [`PERF.md`](PERF.md) for latency percentiles.

---

## Bullet 1 — Infrastructure (LFQueue + cycle-accurate instrumentation)

> Built a from-scratch C++20 low-latency electronic trading ecosystem
> (matching engine + UDP multicast market data + TCP order entry +
> algorithmic trading client + tape-replay backtest harness) following
> Ghosh's *Building Low Latency Applications with C++* (~8,200 LOC).
> Every cross-thread edge is a single-producer / single-consumer
> lock-free `LFQueue<T>` ring buffer; zero allocation on the hot path
> via `MemPool<T>` + placement-new reuse. **RDTSC cycle deltas** at
> every hot function with per-tag log2-bucket latency histograms —
> `TradeEngine::algoOnOrderBookUpdate` hot path measured at **218 ns
> p99** on macOS-Darwin with the `THREAD_AFFINITY_POLICY` hint. (See
> [`PERF.md`](PERF.md))

## Bullet 2 — Infrastructure (measured component optimizations)

> Quantified hot-path optimizations against scientific benchmarks: (a)
> replaced a per-char async logger with a block-copy variant backed by
> an `LFQueue<LogElement>` + dedicated drain thread, measured **54×
> throughput improvement** on 100k-line traces; (b) gated
> `MemPool::allocate/deallocate` ASSERTs on `NDEBUG`, measured **25×
> faster** alloc/dealloc under release. macOS thread affinity via
> `thread_policy_set(THREAD_AFFINITY_POLICY)` delivers a measured
> **11.6× max-jitter reduction** vs unpinned (honest "this isn't Linux
> `isolcpus`" framing). Also diagnosed and fixed a 22-minute Logger
> shutdown hang traced to an SPSC ring-buffer missing producer
> backpressure — one-line fix bounded shutdown to a 5 s deadline.
> (See [`PERF.md`](PERF.md) and [`RESULTS.md`](RESULTS.md) §7)

## Bullet 3 — Research (v1.1 — Avellaneda-Stoikov + OFI on real Binance data)

> Implemented v1.1 inventory-aware market-making on top of the v1.0
> book stack: closed-form Avellaneda-Stoikov reservation price
> (`r = mid − q·γ·σ²·τ + β·OFI`), Cont-Kukanov-Stoikov order-flow
> imbalance alpha overlay, queue-position hysteresis in the
> `OrderManager`, and adaptive clip sizing. Validated on full 24h
> Binance USD-M perp L1 tape across **3 symbols × 4 strategy variants**
> (BTC / ETH / SOL, 2024-03-28). On the directional trading day used,
> v1.1 `full` cut portfolio loss vs the v1.0 threshold-pennying baseline
> from **-$74.7M to -$45.2M (-39.5%)**, with inventory σ reduced by a
> mean ~5× across symbols. Symbol-dependent: on SOL's 0.001 tick, the
> hysteresis dead-zone hurts fill capture and `as_ofi` wins instead of
> `full` — a real sensitivity finding the sweep surfaced.

## Bullet 4 — Research (v1.2 — defensive overlay with honest attribution)

> Designed and shipped v1.2 — a six-feature defensive overlay targeted
> at toxic-flow days where v1.1 is structurally adverse-selected:
> **VPIN** regime detector (Easley/López de Prado/O'Hara 2012),
> **OFI + microprice killswitch** (Cartea/Jaimungal/Penalva 2015 §10.4),
> **regime-aware γ** (dual-EWMA σ_short/σ_long), **asymmetric OFI
> spread widening**, **Stoikov micro-price** anchor (Stoikov 2018), and
> **per-fill maker rebate** booked in `PositionKeeper`. Measured on the
> same 24h tape: **portfolio loss cut from -$45.2M to -$9.7M (78.6%
> reduction)**, with SOL essentially flat (-$262K, 97% reduction).
> Critically: explicit decomposition of the gain shows **~62% is
> algorithmic (killswitch + asymmetric widening + better quotes) and
> ~38% is the maker-rebate accounting credit** — the like-for-like
> algorithmic delta with both strategies booking rebate is +$32.8M
> (≈19% loss reduction). The decomposition is in
> [`RESULTS.md`](RESULTS.md) §5 and is the bullet I'd flag in an
> interview as the rigour signal.

## Bullet 5 — Research (microstructure-honest backtest harness)

> Built a tape-replay backtest harness (`backtest/backtest_engine.cpp`)
> that drives the *same* `Trading::MarketMaker` + `OrderManager` +
> `FeatureEngine` + `PositionKeeper` code path used live, against real
> Binance tape. The strategy stack is byte-for-byte identical to the
> live deployment — only the exchange round-trip is swapped for a
> queue-aware in-process fill simulator with no look-ahead. Per-tick PnL
> emitted to a 13-column CSV (mid, position, real/unreal PnL, volume,
> fills, requotes, σ, OFI). Across the 15-cell sweep (3 symbols × 5
> strategies), all v1.1 strategies reproduced prior runs to within
> ±0.15% drift (simulator OID/memory-layout noise), validating
> determinism of the strategy code itself. Documented limitations:
> simulated fills ≠ live adverse selection, ~50-200 ms WS-RTT gap when
> going live.

## Bullet 6 — Engineering (live bug-hunt under sanitizers)

> Used real Binance L1 data to surface and fix a deterministic SIGSEGV
> in `MarketOrderBook::addOrdersAtPrice` that fired on SOLUSDT but not
> BTC or ETH. Root cause via AddressSanitizer + UBSan was a two-part
> bug: (1) a per-symbol `tick_size` mismatch in the harness that
> `std::llround`'d Binance's bid (186.446) and ask (186.447) to the
> same integer tick, collapsing both sides into one physical price;
> (2) `priceToIndex` keyed by price alone (no Side discriminator), so
> a same-price collision routed a new BUY order into a SELL order's
> chain → silent prev/next corruption → null deref. Fixed in three
> places (script tick table, harness cancel-add ordering,
> `ME_MAX_PRICE_LEVELS` bumped 256 → 65536) with a separate
> `cmake-build-asan/` build so the production binary stayed intact.
> The bug hid under synthetic-tape tests with a ±100-tick range — only
> real-exchange data with finer ticks exposed it.

## Bullet 7 — Integration (LFQueue I/O decoupling + Binance live scaffold)

> Architected the trading client so the strategy layer (TradeEngine,
> MarketMaker, OrderManager, RiskManager) is decoupled from I/O via
> three `LFQueue<T>` channels — the same C++ runs against the
> in-process exchange (loopback TCP + UDP multicast) or, with a
> one-line wiring change, against a Binance Testnet REST + WebSocket
> adapter. Shipped a non-functional drop-in scaffold
> ([`trading/order_gw/binance/`](trading/order_gw/binance/)) with **26
> explicit `TODO(part2):` markers** naming Boost.Beast / OpenSSL /
> nlohmann-json at each WS / HMAC / JSON / rate-limit touchpoint.
> Compiles cleanly as a separate CMake target without affecting the
> default build.

## Bullet 8 — Forward-looking (DeepLOB replacement for `FeatureEngine`)

> Scoped a from-scratch DeepLOB (Zhang/Zohren/Roberts 2019) build to
> replace the hand-crafted `FeatureEngine`'s 6 scalar features (σ, OFI,
> VPIN, micro-price, …) with a learned 10-level LOB representation,
> with the spec at [`docs/DEEPLOB.md`](docs/DEEPLOB.md). Architected
> for a **clean A/B**: precompute the DL micro-price offline in PyTorch
> from L2 data, expose via `FeatureEngine::getDLMicroPred()`, and swap
> only the reservation-price anchor in `MarketMaker` —
> `fair_price = cfg.use_dl_signal_ ? fe->getDLMicroPred() : fe->getMktPrice()`.
> Everything else (killswitch, AS optimal spread, asymmetric widening,
> hysteresis, adaptive clip, rebate) held constant, so the resulting
> `v12_all_on_dl` vs `v12_all_on` head-to-head isolates the value of
> the learned signal from every other v1.2 lever. Plan covers the L1→L2
> data path (Binance `bookDepth` for 2024-03-28), FI-2010 model-validity
> checkpoint (expect F1 ≈ 0.78–0.80 to confirm correct implementation
> before transferring to crypto), and the 18-backtest sweep that
> extends `RESULTS.md` with the comparison. **Frames the result honestly
> upfront**: most likely finding is that DL signal predicts direction
> better than OFI yet shows small/zero P&L lift, because v1.2's
> killswitch already harvests most of the toxic-flow headroom — that
> result is itself the publishable insight of the LOB-DL literature.

---

## Numbers cheat-sheet (post-run)

After `bash scripts/run_full_sweep.sh` (≈ 6h) + `jupyter nbconvert --execute
notebooks/showcase_analysis.ipynb --to notebook --inplace`, the numbers
above are sourced from:

| Number | Source |
|---|---|
| 8,200 LOC | `cloc trading common exchange backtest --exclude-dir=cmake-build-release` |
| 218 ns p99 | `docs/latency_summary.csv` → `Trading_TradeEngine_algoOnOrderBookUpdate_` p99 |
| 54× / 25× / 11.6× | `benchmarks/{logger,release,jitter}_benchmark`; see [`PERF.md`](PERF.md) |
| -$74.7M → -$45.2M (v1.0 → v1.1) | [`RESULTS.md`](RESULTS.md) §4 per-layer table, portfolio row |
| -$45.2M → -$9.7M (v1.1 → v1.2) | [`RESULTS.md`](RESULTS.md) §3.4 portfolio totals |
| 78.6% / 62% algo / 38% rebate | [`RESULTS.md`](RESULTS.md) §5 decomposition |
| ±0.15% reproducibility | [`RESULTS.md`](RESULTS.md) §6 reproducibility check |
| 26 TODOs | `grep -rn 'TODO(part2):' trading/order_gw/binance/ \| wc -l` |
| 5 s drain deadline | `common/logging.h:95-98` |

The DeepLOB bullet refers to a planned build — the spec exists at
`docs/DEEPLOB.md` but the model itself is not yet implemented. The
bullet's "spec at" framing reflects this honestly; remove or edit if
the role expects only shipped work.
