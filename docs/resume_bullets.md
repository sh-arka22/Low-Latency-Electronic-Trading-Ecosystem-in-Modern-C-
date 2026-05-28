# Resume bullets — `electronic_trading_ecosystem`

Six drop-in bullets. Three infra-leaning, two research-leaning, one
integration. Numbers come from the showcase run (`bash
scripts/run_showcase.sh`). Regenerated whenever the showcase notebook is
re-executed.

Pick 3–5 depending on the role you're applying to:
- **HFT-infra / low-latency C++** → bullets 1, 2, 6, plus 5
- **Quant-researcher / strategy** → bullets 3, 4, 5
- **Quant-dev hybrid** → bullets 1, 3, 5, plus 6 (debug story)

---

## Bullet 1 — Infrastructure (LFQueue + cycle-accurate instrumentation)

> Built a from-scratch C++20 low-latency electronic trading ecosystem
> (matching engine + UDP multicast market data + TCP order entry +
> algorithmic trading client) following Ghosh's *Building Low Latency
> Applications with C++* (~7,292 LOC). Every cross-thread edge is a
> single-producer / single-consumer lock-free `LFQueue<T>` ring buffer;
> zero allocation on the hot path via `MemPool<T>` + placement-new
> reuse. RDTSC cycle deltas measured at every hot function with per-tag
> log2-bucket latency histograms — `TradeEngine::algoOnOrderBookUpdate`
> hot path measured at **218 ns p99** on macOS-Darwin with the
> `THREAD_AFFINITY_POLICY` hint. (See [`PERF.md`](PERF.md))

## Bullet 2 — Infrastructure (measured component optimizations)

> Quantified two hot-path optimizations against scientific benchmarks:
> (a) replaced a per-char async logger with a block-copy variant
> backed by an `LFQueue<LogElement>` + dedicated drain thread,
> measured **54× throughput improvement** on 100k-line traces;
> (b) gated `MemPool::allocate/deallocate` ASSERTs on `NDEBUG`,
> measured **26× faster** alloc/dealloc under release. macOS thread
> affinity via `thread_policy_set(THREAD_AFFINITY_POLICY)` delivers a
> measured **11.6× max-jitter reduction** vs unpinned (honest "this
> isn't Linux `isolcpus`" framing). (See [`PERF.md`](PERF.md))

## Bullet 3 — Research (Avellaneda-Stoikov + OFI on real Binance data)

> Implemented a v1.1 inventory-aware market-making strategy on top of
> the v1.0 stack: closed-form Avellaneda-Stoikov reservation price
> (`r = mid − q·γ·σ²·τ + β·OFI`), Cont-Kukanov-Stoikov order-flow
> imbalance alpha overlay, queue-position hysteresis in the
> `OrderManager`, and adaptive clip sizing. Validated on real Binance
> USD-M perp L1 tape across **3 symbols × 4 strategy variants** (12
> cells, BTCUSDT / ETHUSDT / SOLUSDT, 2024-03-28): inventory volatility
> reduced by a mean **5.7×** (baseline → winner) across symbols,
> fee-adjusted drawdown reduced by **33%** with the 2-bp maker fee
> floor. Best strategy is symbol-dependent — `full` (AS+OFI+hyst+aclip)
> wins on BTC and ETH, but at SOL's fine 0.001 tick the hysteresis
> dead-zone hurts fill capture and `as_ofi` wins instead. **A real
> hyperparameter sensitivity finding the showcase surfaced.**
> (See [`docs/showcase.md`](docs/showcase.md))

## Bullet 4 — Research (microstructure-honest backtest)

> Built a tape-replay backtest harness (`backtest/backtest_engine.cpp`)
> that drives the *same* `Trading::MarketMaker` code path used live,
> against real Binance L1 tape. The strategy stack
> (TradeEngine + FeatureEngine + PositionKeeper + OrderManager +
> RiskManager) is byte-for-byte identical to the live deployment —
> only the I/O edge is swapped. Fills are simulated via a queue-aware
> BBO-crossing model with no look-ahead; PnL is logged per ~50 ms
> snapshot to a per-strategy CSV (13-column schema: mid, position,
> real/unreal PnL, volume, fills, requotes, σ, OFI). Across the
> 12-cell sweep, **fee-adjusted PnL ranged from -13.71M to -7.02M**
> quote units; the doc explicitly enumerates limitations (simulated
> fills ≠ live adverse selection, ~50-200 ms WS-RTT gap when going
> live, AS toxicity exposure per *Improving AS with RL* PLOS One 2023).

## Bullet 5 — Integration (LFQueue I/O decoupling + live-trading scaffold)

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

## Bullet 6 — Engineering (live bug-hunt under sanitizers)

> Used the L1-data backtest to surface and fix a deterministic SIGSEGV
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
> The bug had hidden under synthetic-tape tests that clamped to a
> ±100-tick range — only real-exchange data with finer ticks exposed
> it.

---

## Numbers cheat-sheet (post-run)

After `bash scripts/run_showcase.sh && jupyter nbconvert --execute
notebooks/showcase_analysis.ipynb --to notebook --inplace`, the slots
above are sourced from:

| Slot | Source |
|---|---|
| 7,292 LOC | `cloc trading common exchange backtest --exclude-dir=cmake-build-release` |
| 218 ns p99 | `docs/latency_summary.csv` → `Trading_TradeEngine_algoOnOrderBookUpdate_` p99 |
| 5.7× inv σ | mean of (BTC 7.9×, ETH 3.2×, SOL 6.0×) baseline → winner |
| 33% fee-adj | mean (BTC 41%, ETH 30%, SOL 28%) less negative vs baseline |
| -13.71M, -7.02M | min/max fee-adjusted PnL across 12 cells |
| 26 TODOs | `grep -rn 'TODO(part2):' trading/order_gw/binance/ \| wc -l` |
| 2 bp | `scripts/analyze_pnl.py::MAKER_FEE` × 10000 (default 2.0) |
