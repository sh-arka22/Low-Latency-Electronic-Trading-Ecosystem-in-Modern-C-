# Learning Map — Electronic Trading Ecosystem

A first-principles study plan for understanding *your own* system end to end: how each
piece works, why it exists, where it lives in the code, and what you changed to improve it
across v1.0 → v1.1 → v1.2.

This is the **roadmap**. It gives you the breadth (every concept, in dependency order) and a
pointer into the code for each. We then go **deep on one module at a time** in follow-up
sessions — that's the "one by one" part.

---

## How to use this map

Your learning style is BFS-then-DFS: skim the whole frontier first, then descend into each
node. This document is built for exactly that.

1. **Breadth pass (1 sitting).** Read Tier 0 and the one-line "idea" of every module below.
   Goal: hold the whole system as a single mental picture. Don't open code yet.
2. **Depth pass (one module per sitting).** Pick the next module in order. For a deep-dive we
   will: (a) derive the concept from first principles, (b) read the actual code together,
   (c) you answer the "Master it when you can…" questions from memory, (d) trace it through
   one real trade.
3. **Order matters.** Modules are sorted so each one only depends on earlier ones. Don't skip
   forward — M21 (Avellaneda-Stoikov) won't land until M17 (PositionKeeper) and M20
   (FeatureEngine) are solid.

Each module entry has the same five fields:

- **Idea** — the concept from first principles (trading-specific; assumes you know C++ + basic probability).
- **Why it exists** — the problem it solves / what breaks without it.
- **Code** — the files and symbols to read.
- **Master it when you can…** — the questions that prove you understand it.
- **Your build / improvement** — what was v1.0 (book) vs. what *you* added in v1.1 / v1.2.

---

## Tier 0 — The whole system in one breath (do this first)

**What this project is.** A complete electronic market on one machine: an **exchange side**
(matching engine + market-data feeds + order-entry server) and a **trading client side**
(market-data consumer + order gateway + an algorithmic market-making strategy + a backtest
harness). The two halves talk over real kernel sockets on loopback — TCP for orders, UDP
multicast for market data — so the wire formats, threads, and queue boundaries are identical
to a two-machine production setup.

**The one organizing principle.** Every major component is its own OS thread. *Everything*
crossing a thread boundary goes through an `LFQueue<T>` — a pre-allocated, lock-free,
single-producer/single-consumer ring buffer. **There is no mutex or condition variable on any
hot path.** Once you internalize "thread-per-component, LFQueue between them, zero allocation
after warmup," the entire architecture collapses into something you can draw from memory.

**The round trip (memorize this skeleton).** A single fill crosses ~8 threads and 6 queues:

```
   MARKET DATA IN                    DECISION                 ORDER OUT
NIC ─► MarketDataConsumer ─► TradeEngine ─► FeatureEngine ─► MarketMaker
                                  │                              │
                                  └─ PositionKeeper (mark)        ▼
                                                            OrderManager ─► RiskManager
                                                                 │
   FILL HOME                                                      ▼
OrderServer ◄─ MatchingEngine ◄─ FIFOSequencer ◄─ OrderServer ◄─ OrderGateway ◄─ TradeEngine
   │                  │
   └─► PositionKeeper.addFill ◄── OrderGateway ◄── (TCP response)
```

The full 5-phase walkthrough with TTT timestamps is in **`README.md` → "How a real trade
flows through the system."** Read it once now for breadth; we'll return to it per module.

### How you built it (the chronology — your "what I changed" thread)

The codebase grew in three layers. Knowing the order is half the understanding:

- **v1.0 — Infrastructure (book ch. 5–12).** Faithful re-implementation of Sourav Ghosh's
  *Building Low Latency Applications with C++*. This is the matching engine, order book, MD
  feeds, order entry, lock-free queues, mem pool, async logger. *Goal: learn how the pieces
  fit.* Git: the `ch7…ch12` commits.
- **v1.1 — Inventory-aware market maker.** You replaced the book's naive "penny the touch"
  quoter with a real strategy: Avellaneda-Stoikov reservation + spread, Cont-Kukanov-Stoikov
  OFI overlay, queue-position hysteresis, adaptive clip sizing. Plus a **tape-replay backtest
  harness** so the *same* strategy code runs against real Binance data. Git: `v1.1:` commits +
  the `day1/day4/day6/day7` perf-polish commits.
- **v1.2 — Defensive overlay.** Targeted at the toxic-flow days where v1.1 gets adversely
  selected. You added VPIN toxicity detection, an OFI/microprice killswitch, regime-adaptive γ,
  asymmetric spread widening, a Stoikov micro-price anchor, and per-fill maker rebate
  accounting. This is the layer measured in `RESULTS.md` (portfolio loss cut 78.6%). Git:
  `v1.2:` commit + the `feat(scripts)`, `feat(backtest)`, `feat(trading)` commits.

The deep-dive modules below are tagged `[v1.0]` / `[v1.1]` / `[v1.2]` so you always know which
layer — and therefore which of *your* decisions — you're studying.

---

## Tier 1 — Low-latency foundations (`common/`)

These are the building blocks every other tier sits on. Master them first; nothing trading-
specific makes sense until "why lock-free, why no malloc, why p99 not mean" is reflexive.

### M01 — Why latency at all? Tail latency and the cost model `[v1.0]`
- **Idea.** In trading, the distribution's *tail* (p99/p99.9) is what loses money, not the
  mean — one slow quote during a fast market is an adverse fill. You reason about cost in
  nanoseconds: an L1 hit (~1 ns) vs. cache miss (~100 ns) vs. syscall (~1 µs) vs. `malloc` or
  a lock under contention (microseconds, unbounded).
- **Why it exists.** It's the *why* behind every design choice in `common/`. If you can't
  rank those costs, the rest looks like premature optimization.
- **Code.** Conceptual — but see `PERF.md` for the percentile tables this mindset produces.
- **Master it when you can…** explain why a market maker cares about p99 over mean; estimate
  the latency budget for one BBO→quote cycle; say why jitter (variance) is itself a cost.
- **Your build.** Framing module — the lens for everything below.

### M02 — `LFQueue<T>`: the lock-free SPSC ring buffer `[v1.0]`
- **Idea.** A fixed-size ring buffer with one producer thread and one consumer thread. Because
  there's exactly one of each, you need no locks — just two indices and the right memory
  ordering so the consumer never reads an entry before the producer's write is visible.
- **Why it exists.** It's the *only* inter-thread channel in the system. SPSC (not MPMC) is a
  deliberate constraint: it's the fastest possible queue and it forces a clean one-way
  dataflow between components.
- **Code.** `common/lf_queue.h`.
- **Master it when you can…** explain why SPSC needs no CAS; identify which memory orderings
  guard the read/write indices and why; describe what happens when the queue is full (back-
  pressure) and why that bit you in the logger (see M04).
- **Your build.** `[v1.0]` book infra — but the unfixed back-pressure issue surfaced in your
  v1.2 sweep (M04, `RESULTS.md` §7).

### M03 — `MemPool<T>` and the zero-allocation hot path `[v1.0]`
- **Idea.** Pre-allocate a pool of `T` objects up front; `allocate()` hands back a free slot
  and `placement new`-constructs in place; `deallocate()` marks it free. No `malloc` on the
  per-event path → no allocator lock, no page faults, no unpredictable latency.
- **Why it exists.** Orders, market updates, and log elements are created/destroyed millions
  of times. Heap allocation there would dominate the latency tail.
- **Code.** `common/mem_pool.h`; optimized variant `common/opt_mem_pool.h`.
- **Master it when you can…** explain placement new vs. `new`; explain why the two `ASSERT`s in
  `allocate`/`deallocate` are gated behind `NDEBUG`; quote the measured speedup.
- **Your build / improvement.** `[v1.0]` pool; the **NDEBUG-gated asserts give ~25× faster
  alloc/dealloc in release** — your `benchmarks/release_benchmark.cpp` proves it.

### M04 — The async lock-free Logger `[v1.0 + fix]`
- **Idea.** The hot path must never touch disk. Producers push `LogElement`s into an
  `LFQueue<LogElement>`; a dedicated drain thread serializes them to a file. I/O cost moves
  off the critical path entirely.
- **Why it exists.** A single `fprintf` (microseconds, occasionally milliseconds on flush)
  inside the matching loop would wreck the latency profile.
- **Code.** `common/logging.{h,cpp}`; block-copy string variant `common/opt_logging.h`.
- **Master it when you can…** explain why the block-copy variant is **54× faster** than per-
  char logging (`benchmarks/logger_benchmark.cpp`); explain the shutdown-drain deadlock you
  hit and fixed.
- **Your build / improvement.** `[v1.0]` design; **you fixed `Logger::flushQueue` ignoring
  `running_` during shutdown** — it hung the v1.2 sweep ~22 min/backtest. One-line fix; the
  underlying LFQueue back-pressure cause is documented but unfixed (`RESULTS.md` §7).

### M05 — Threading model & CPU affinity `[v1.0 + your Day-6 work]`
- **Idea.** Pin each component thread to a core so it isn't migrated mid-hot-loop (migration =
  cold cache + scheduler jitter). On Linux that's a hard pin (`pthread_setaffinity_np`); on
  macOS only a *hint* (`thread_policy_set`).
- **Why it exists.** Jitter is a cost (M01). Pinning trades fairness for predictability.
- **Code.** `common/thread_utils.h` (`pinCurrentThreadDarwinHint`).
- **Master it when you can…** explain why affinity reduces tail latency; explain honestly why
  the macOS hint is *not* `isolcpus`; quote the measured jitter reduction.
- **Your build / improvement.** `[v1.1/Day6]` You added the Darwin affinity path + an RDTSC
  `jitter_benchmark` and measured a **11.6× reduction in max jitter** pinned vs. unpinned
  (`docs/jitter.png`, `PERF.md` §3). The honesty about "this isn't a hard pin" is yours.

### M06 — Time, types, and sentinels `[v1.0 + a bug you fixed]`
- **Idea.** Strong typedefs (`TickerId`, `OrderId`, `Price`, `Qty`, `Side`) make the wire
  format and the books self-documenting and mis-assignment-resistant. Each has an `_INVALID`
  sentinel. Nanosecond timestamps come from `time_utils.h`.
- **Why it exists.** A trading system is a pipeline of typed messages; weak `int` typing is how
  you cross a price into a qty field at 3 a.m.
- **Code.** `common/types.h`, `common/time_utils.h`, `common/macros.h` (`LIKELY`/`UNLIKELY`,
  `ASSERT`).
- **Master it when you can…** list the core types and their sentinels; explain why `NaN` is a
  *dangerous* sentinel (`NaN != NaN` is true) — the bug that poisoned your volatility (M20).
- **Your build / improvement.** `[v1.0]` types; the **NaN-sentinel volatility bug** was yours
  to find and fix (switch to `std::isnan`) — see M20.

### M07 — Sockets: TCP order entry & UDP multicast market data `[v1.0]`
- **Idea.** Orders go over **TCP** (reliable, ordered, per-client) — you can't drop an order.
  Market data goes over **UDP multicast** (one publish reaches all subscribers, no per-client
  fan-out cost) — and you accept that you must handle gaps yourself (M12/M13).
- **Why it exists.** The reliability/scalability trade-off is the reason exchanges use exactly
  this split.
- **Code.** `common/tcp_socket.{h,cpp}`, `common/tcp_server.{h,cpp}`,
  `common/mcast_socket.{h,cpp}`, `common/socket_utils.h`.
- **Master it when you can…** explain why orders=TCP and MD=UDP-multicast; explain the
  `poll()` fallback for `epoll` and `SO_REUSEPORT` (your macOS-compat additions).
- **Your build.** `[v1.0]` — with macOS-friendly `poll()` + `SO_REUSEPORT` so one source tree
  runs on a dev laptop and a Linux target.

---

## Tier 2 — The exchange (build the venue you trade against)

### M08 — Wire protocol & message types `[v1.0]`
- **Idea.** Bespoke `#pragma pack(1)` structs for `ClientRequest`/`ClientResponse`
  (order entry) and `MarketUpdate` (public feed). Packed = no padding = a struct *is* the wire
  format. Sequence numbers ride on every message for gap detection.
- **Why it exists.** You need a contract between exchange and client. Bespoke (not FIX/SBE)
  keeps it readable and laptop-scoped.
- **Code.** `exchange/order_server/client_request.h`, `client_response.h`,
  `exchange/market_data/market_update.h`.
- **Master it when you can…** name the fields of a `NEW` request and a fill response; explain
  why `#pragma pack(1)` matters on the wire; explain how sequence numbers enable recovery.
- **Your build.** `[v1.0]`; you extended `MarketUpdateType` for snapshot recovery.

### M09 — The limit order book (data structure) `[v1.0]`
- **Idea.** A price-time-priority book: each price level holds a FIFO queue of resting orders;
  levels are kept sorted per side. Add inserts at the tail of a level; cancel unlinks in O(1)
  via an order-id map; the best bid/ask are the front levels.
- **Why it exists.** It's the core state of any venue — everything else (matching, MD) reads
  or mutates it.
- **Code.** `exchange/matcher/me_order_book.{h,cpp}`, `exchange/matcher/me_order.{h,cpp}`.
- **Master it when you can…** draw the structure (levels ↔ intrusive order lists ↔ id map);
  state the complexity of add/cancel/lookup; explain why FIFO-within-level encodes time
  priority.
- **Your build.** `[v1.0]`; the array/hash baseline is benchmarked in `hash_benchmark.cpp`.

### M10 — The matching engine `[v1.0]`
- **Idea.** Drains client requests; for a `NEW`, walk the opposite side from the touch,
  matching while prices cross, emitting a private fill response to *each* side's owner and a
  public TRADE update; queue any residual as a passive resting order. `CANCEL` removes and
  emits a cancel + public delta.
- **Why it exists.** This is the venue's heart — price-time priority is *enforced here*.
- **Code.** `exchange/matcher/matching_engine.{h,cpp}` (`processClientRequest`,
  `sendClientResponse`, `sendMarketUpdate`).
- **Master it when you can…** trace a marketable `NEW` that partially fills against two resting
  orders; say exactly which messages leave the engine and to whom; explain why matching is
  single-threaded.
- **Your build.** `[v1.0]`; hot path instrumented with `Exchange_MEOrderBook_match` RDTSC
  (M32).

### M11 — Order server + FIFO sequencer `[v1.0]`
- **Idea.** TCP server accepting many client connections; the `FIFOSequencer` stamps requests
  by arrival time so that under concurrency, per-client ordering and cross-client fairness are
  preserved before requests hit the (single-threaded) matcher.
- **Why it exists.** Without arrival-time fairness, faster-connected clients could jump the
  queue — a correctness/fairness property, not just performance.
- **Code.** `exchange/order_server/order_server.{h,cpp}`, `fifo_sequencer.h`.
- **Master it when you can…** explain what the sequencer orders and why; trace a response from
  the matcher back to the *originating* client's socket.
- **Your build.** `[v1.0]`.

### M12 — Market-data publisher + snapshot synthesizer `[v1.0]`
- **Idea.** Two multicast streams: an **incremental** feed of book deltas (every change) and a
  periodic **full-book snapshot** on a parallel group. A client that just joined — or that
  detected a gap — uses the snapshot to rebuild state, then resumes on the incremental.
- **Why it exists.** UDP can drop packets; the snapshot is the recovery mechanism that makes a
  lossy transport usable.
- **Code.** `exchange/market_data/market_data_publisher.{h,cpp}`,
  `snapshot_synthesizer.{h,cpp}`.
- **Master it when you can…** explain the incremental-vs-snapshot split; describe how a client
  resynchronizes after a dropped packet; say why the snapshot runs on its own group.
- **Your build.** `[v1.0]`.

---

## Tier 3 — The trading client (consume, decide, send)

### M13 — Market-data consumer + gap recovery `[v1.0]`
- **Idea.** Subscribes to the incremental feed, validates sequence numbers, and on a gap joins
  the snapshot feed, buffers *both* in `std::map` (sorted by seq), and drains the merged,
  ordered stream into the strategy.
- **Why it exists.** It's the client-side mirror of M12 — turns a lossy multicast into a clean,
  ordered event stream.
- **Code.** `trading/market_data/market_data_consumer.{h,cpp}`.
- **Master it when you can…** describe the exact state machine on a detected gap; explain why
  both feeds are buffered and merged rather than just switched.
- **Your build.** `[v1.0]`.

### M14 — Order gateway `[v1.0]`
- **Idea.** The client-side mirror of the order server: one persistent TCP connection, drains
  the outgoing-request LFQ and writes packed requests, parses incoming responses and pushes
  them onto the response LFQ.
- **Why it exists.** It's the only path orders take to the exchange and fills take back.
- **Code.** `trading/order_gw/order_gateway.{h,cpp}` (`run`, `recvCallback`).
- **Master it when you can…** explain how it preserves request order; trace a response from
  socket to response LFQ.
- **Your build.** `[v1.0]`; v1.2 adds a Binance live WS adapter alongside (`trading/order_gw/binance/`).

### M15 — Client-side order book mirror `[v1.0 + a bug you fixed]`
- **Idea.** The client maintains its own `MarketOrderBook` per ticker, rebuilt from the MD
  feed, and recomputes the BBO on each update. Strategies key entirely off this mirror.
- **Why it exists.** The strategy needs a local, queryable view of the market; it can't call
  back to the exchange per decision.
- **Code.** `trading/strategy/market_order_book.{h,cpp}` (`onMarketUpdate`, `updateBBO`).
- **Master it when you can…** explain the cold-start bug: `onMarketUpdate` skipped `updateBBO`
  on the very first ADD (the `bid_updated` check read `bids_by_price_` *before* the add), so
  strategies keyed off BBO never quoted — caught when the first backtest showed zero fills.
- **Your build / improvement.** `[v1.0]` structure; **the first-ADD BBO bug was yours to find
  and fix.**

### M16 — `TradeEngine`: the orchestrator `[v1.0 + your Day-4 perf work]`
- **Idea.** The strategy thread's hot loop. Round-robin drains the response LFQ + MD LFQ; for
  each MD update applies it to the per-ticker book and fans out callbacks: `PositionKeeper`
  (mark), `FeatureEngine` (recompute signals), then the active algo (`MarketMaker` /
  `LiquidityTaker`).
- **Why it exists.** It's the spine of the client — the single place market events become
  decisions.
- **Code.** `trading/strategy/trade_engine.{h,cpp}` (`run`, `onOrderBookUpdate`,
  `onOrderUpdate`, `sendClientRequest`).
- **Master it when you can…** name the two input queues and one output queue; list the three
  callbacks fired per BBO update *in order*; explain why their order matters.
- **Your build / improvement.** `[v1.0]` orchestrator; **Day 4 you replaced the book's
  `std::function<void(...)>` algo dispatch with direct `mm_algo_->onOrderBookUpdate(...)`
  calls** — `nm trading_main | grep 'std::function<void'` → 0 lines. Removes an indirect call +
  potential heap from the hot path.

### M17 — `PositionKeeper`: position & PnL `[v1.0 + v1.2 rebate]`
- **Idea.** Per-ticker position, realized + unrealized PnL, and entry VWAPs. Marks to the BBO
  on every book update; books realized PnL on every fill.
- **Why it exists.** Inventory `q` and PnL are *inputs to the strategy* (Avellaneda-Stoikov
  skews quotes by `q`) and to risk (M18). Without it the MM is flying blind.
- **Code.** `trading/strategy/position_keeper.h` (`updateBBO`, `addFill`).
- **Master it when you can…** explain realized vs. unrealized PnL and how VWAP entry tracks
  them; explain mark-to-market on BBO.
- **Your build / improvement.** `[v1.0]` keeper; **`[v1.2]` you added the per-fill maker
  rebate**: `position_keeper.h:88-95`, `real_pnl += notional · maker_rebate_bps · 1e-4`
  (positive bps = rebate, negative = fee) — makes backtest PnL reflect venue economics.

### M18 — `RiskManager`: pre-trade gating `[v1.0]`
- **Idea.** Before any `NEW` is sent, check three limits: max order size, projected position
  after the order, and a PnL floor. Returns an enum the OrderManager must honor.
- **Why it exists.** A bug in the strategy must not be able to build an unbounded position. Risk
  is a gate, not a suggestion.
- **Code.** `trading/strategy/risk_manager.{h,cpp}` (`checkPreTradeRisk`).
- **Master it when you can…** name the three checks; explain the **`max_loss_` gotcha** — the
  test is `total_pnl_ < max_loss_`, so it's a *min-PnL floor*, not a max loss; set `-1e9` to
  disable, and a positive value makes the strategy refuse to trade.
- **Your build.** `[v1.0]`.

### M19 — `OrderManager`: own-order lifecycle + queue hysteresis `[v1.0 + v1.1]`
- **Idea.** Tracks the strategy's *own* resting orders (`OMOrder` state machine: PENDING_NEW →
  LIVE → PENDING_CANCEL → DEAD), one per ticker per side. `moveOrders` reconciles desired vs.
  live quotes, calling risk before any `NEW`.
- **Why it exists.** The strategy thinks in "I want bid X / ask Y"; something must translate
  that into new/cancel messages and track acknowledgements.
- **Code.** `trading/strategy/order_manager.{h,cpp}` (`moveOrders`, `moveOrder`,
  `setHysteresisTicks`), `trading/strategy/om_order.h`.
- **Master it when you can…** draw the OMOrder state machine; explain **queue-position
  hysteresis**: keep the order LIVE while `|target − live| ≤ hysteresis_ticks_` instead of
  cancel/re-add on every tick — re-adding loses FIFO queue priority.
- **Your build / improvement.** `[v1.0]` lifecycle; **`[v1.1]` the hysteresis dead-zone is
  yours** (`order_manager.h:66-78`). It's exchange-dependent — rewards FIFO venues, less so
  pro-rata. This was a measured win in the v1.1 sweep.

---

## Tier 4 — The strategy & the quant math (the brain)

### M20 — `FeatureEngine`: microstructure signals `[v1.0 base + v1.1/v1.2 signals]`
- **Idea.** Recomputes, incrementally, on every tick: EWMA realized volatility σ on mid-price
  returns, a long-horizon σ for regime detection, OFI, micro-price, and VPIN. Every signal is
  O(1) per update — no windows to re-scan.
- **Why it exists.** These are the *inputs* to every quoting decision. The quality of the MM is
  capped by the quality of these features.
- **Code.** `trading/strategy/feature_engine.h` (`onOrderBookUpdate`, `onTradeUpdate`).
- **Master it when you can…** write the EWMA update `var = λ·var + (1−λ)·ret²`; explain the
  bootstrap (skip σ until `vol_bootstrap_` samples); explain the NaN-sentinel poison bug
  (M06) and the `std::isnan` fix.
- **Your build / improvement.** `[v1.0]` had VWAP-of-touch micro-price only. **`[v1.1]` you
  added EWMA σ + OFI; `[v1.2]` added long-horizon σ (regime), Stoikov micro, and VPIN**, all
  made tunable by lifting the constants into `TradeEngineCfg`.

### M21 — Avellaneda-Stoikov market making `[v1.1]` ★ core quant module
- **Idea.** The optimal quotes for an inventory-averse MM. With inventory `q`, risk-aversion
  `γ`, mid `s`, volatility `σ`, time-to-horizon `τ`, arrival intensity `κ`:
  `reservation = s − q·γ·σ²·τ` (skew quotes *away* from your inventory) and
  `spread = γ·σ²·τ + (2/γ)·ln(1 + γ/κ)` (wider when riskier / less liquid). Bid/ask =
  reservation ∓ spread/2.
- **Why it exists.** It replaces the book's "penny the touch" heuristic with a quoter that
  actually prices in inventory, volatility, and a session clock — the three things a real MM
  must.
- **Code.** `trading/strategy/market_maker.{h,cpp}` (the `cfg.use_as_` block); math mapped in
  `STRATEGY.md`. Source: Avellaneda & Stoikov (2008).
- **Master it when you can…** derive *why* the reservation price skews with `q` (you hold
  inventory → you want to offload → shade quotes); explain each term of the spread; say what
  `γ`, `κ`, `τ` each control and how you calibrate γ/κ (`scripts/calibrate_gamma_kappa.py`).
- **Your build / improvement.** `[v1.1]` This is *the* upgrade that turned the book's toy
  quoter into a real strategy. Opt-in via `TradeEngineCfg` so the v1.0 CLI still works.

### M22 — Order Flow Imbalance (Cont-Kukanov-Stoikov 2014) `[v1.1]`
- **Idea.** A short-horizon alpha. Per update, signed change in best-bid depth minus signed
  change in best-ask depth: `e_b` (= +Q_b if bid price rose, ΔQ_b if unchanged, −Q_b,prev if
  fell), `e_a` symmetric, `e_n = e_b − e_a`, EWMA-smoothed. Positive OFI = buy pressure →
  shift quotes up. Added to the reservation as `+ β·OFI`.
- **Why it exists.** Inventory/vol (M21) are defensive; OFI is the one *predictive* signal —
  it nudges the fair price toward where flow says it's going.
- **Code.** `feature_engine.h` (the OFI block, lines ~85-103); applied in `market_maker.cpp`.
- **Master it when you can…** reproduce the three-case `e_b` definition from memory; explain
  why depth *change* (not level) is the signal; explain the sign convention.
- **Your build / improvement.** `[v1.1]` overlay you added on top of AS.

### M23 — Micro-price: VWAP-of-touch vs. Stoikov (2018) `[v1.0 base + v1.2 Stoikov]`
- **Idea.** A better "fair value" than the mid. v1.0 used VWAP-of-touch
  (`(bid·ask_qty + ask·bid_qty)/total_qty` — weights toward the *thin* side). v1.2 switches in
  a Stoikov micro-price approximation: `mid + 0.5·spread·imbalance`, where
  `imbalance = (bid_qty − ask_qty)/total_qty`.
- **Why it exists.** The mid is naive when the book is lopsided; the micro-price is where the
  next trade is likelier to print.
- **Code.** `feature_engine.h` (`use_stoikov_micro_` branch, lines ~48-59). Source: Stoikov
  (2018).
- **Master it when you can…** explain why VWAP-of-touch weights the thin side; explain why
  Stoikov's form captures the "dominant effect" without the offline transition table the full
  2018 method uses.
- **Your build / improvement.** `[v1.2]` the Stoikov anchor is yours (Step 6); v1.0 micro-price
  was VWAP-of-touch.

### M24 — Adaptive clip sizing `[v1.1]`
- **Idea.** Scale order size by inventory headroom and volatility:
  `clip · (1 − |q|/q_max) · clamp(σ_ref/σ, 0.5, 1.5)` — post smaller as you approach the
  position limit or as volatility spikes.
- **Why it exists.** Fixed clip size ignores that the *last* unit of inventory near your limit
  is the most dangerous, and that size in a stressed market is more likely to be adversely
  filled.
- **Code.** `market_maker.cpp` (adaptive-clip block).
- **Master it when you can…** explain each factor; explain why size shrinks near the limit and
  in high vol.
- **Your build / improvement.** `[v1.1]` yours.

### M25 — `LiquidityTaker`: the contrast algo `[v1.0]`
- **Idea.** The opposite stance to a market maker — it *crosses* the spread to take liquidity
  when a signal fires, instead of resting passive quotes. Same `TradeEngine`/`OrderManager`
  plumbing, different decision.
- **Why it exists.** Studying it sharpens what's special about market making (you *earn* the
  spread vs. *pay* it) and shows the dispatch is strategy-agnostic.
- **Code.** `trading/strategy/liquidity_taker.{h,cpp}`.
- **Master it when you can…** contrast maker vs. taker on spread, queue, and adverse selection;
  explain why both plug into the same engine.
- **Your build.** `[v1.0]`.

---

## Tier 5 — The v1.2 defensive overlay (your toxic-flow improvements)

This whole tier is *your* work — the answer to "what did I change to improve it." It targets
the days where the v1.1 quoter gets adversely selected and bleeds.

### M26 — Adverse selection & toxic flow: the problem `[concept]`
- **Idea.** A passive MM is *picked off* when informed traders trade against stale quotes: you
  buy right before the price drops. On toxic days, the spread you earn is dwarfed by these
  adverse fills. This is the failure mode v1.1 has and v1.2 defends against.
- **Why it exists.** Without naming the problem, the six v1.2 techniques look like unrelated
  knobs. They're all one idea: *detect toxicity, then quote less / wider / not at all.*
- **Code.** Conceptual; the payoff is in `RESULTS.md` (portfolio gross PnL −$45.17M → −$9.66M,
  a 78.6% loss reduction).
- **Master it when you can…** explain adverse selection to someone in two sentences; explain
  why *more* trading is worse on a toxic day.
- **Your build.** Framing for everything below.

### M27 — VPIN: flow-toxicity detection `[v1.2]`
- **Idea.** Volume-Synchronized Probability of Informed Trading. Bucket trades into equal-
  *volume* buckets (not equal-time); within each, classify buy vs. sell volume (BVC); VPIN is
  the rolling mean of `|buy − sell| / bucket_size`. High VPIN = one-sided, informed flow.
- **Why it exists.** It's the *trigger* the rest of the overlay reads — the regime detector that
  says "today is toxic."
- **Code.** `trading/strategy/vpin.h` (`class Vpin`, `setBucketSize`, `onTrade`, `value`);
  wired in `feature_engine.h` `onTradeUpdate`. Source: Easley, López de Prado, O'Hara (2012).
- **Master it when you can…** explain why *volume* buckets beat time buckets in fast markets;
  explain BVC classification; explain how VPIN feeds the killswitch and regime-γ.
- **Your build / improvement.** `[v1.2]` Step 7. Note: `STRATEGY.md` lists VPIN as deliberately
  *deferred* from v1.1 — adding it in v1.2 is exactly your "what I changed" story.

### M28 — Killswitch: the OFI / micro-price circuit breaker `[v1.2]`
- **Idea.** Before quoting, if `|OFI| > kill_ofi_eff` OR `|micro − mid| > kill_micro_eff_ticks`,
  cancel all orders and *return* — quote nothing this tick. When VPIN is toxic, the thresholds
  tighten automatically.
- **Why it exists.** The cheapest defense against a toxic burst is to *not be in the market*.
  It's the only early-return path in the decision loop.
- **Code.** `market_maker.cpp:47-76` (`cfg.use_killswitch_`, `kill_ofi_eff`).
- **Master it when you can…** explain the two trigger conditions; explain how VPIN tightens
  them; explain why pulling quotes beats widening them in the worst case.
- **Your build / improvement.** `[v1.2]` Step 4, with the VPIN coupling from Step 7.

### M29 — Regime-adaptive γ `[v1.2]`
- **Idea.** Make risk-aversion `γ` respond to the regime: scale it by
  `clamp(σ_short/σ_long, 1/scale, scale)`. When short-horizon vol spikes above the long-run
  baseline, γ rises → AS spread widens and inventory skew strengthens.
- **Why it exists.** A fixed γ is mis-tuned half the time. Tying it to the σ-ratio makes the
  same strategy defensive in stress and competitive in calm.
- **Code.** `market_maker.cpp` (`use_regime_gamma_`); σ_long from `feature_engine.h`
  (`ewma_variance_long_`, `getVolatilityLong`).
- **Master it when you can…** explain why σ_short/σ_long is a regime signal; explain what the
  clamp prevents (γ exploding/vanishing).
- **Your build / improvement.** `[v1.2]` Step 5; the long-horizon EWMA plumbing was added
  "always-on" so this could materialize it.

### M30 — Asymmetric OFI spread widening `[v1.2]`
- **Idea.** When flow is toxic, widen *only the side the toxicity is hitting*:
  `widen_k · max(±OFI, 0)` on the exposed side, leave the other competitive. Don't symmetric-
  widen and give up the safe side's edge.
- **Why it exists.** Toxic flow is directional. Symmetric widening over-defends and under-
  earns; asymmetric widening defends precisely.
- **Code.** `market_maker.cpp` (asymmetric-widening block). Source: Cartea/Jaimungal/Penalva
  (2015) §10.4.
- **Master it when you can…** explain why only one side should widen; tie the widen direction
  to the sign of OFI.
- **Your build / improvement.** `[v1.2]`.

### M31 — Maker-rebate accounting `[v1.2]`
- **Idea.** Real venues *pay* you for providing liquidity (maker rebate) and charge for taking.
  Credit `notional · rebate_bps · 1e-4` on every passive fill so backtest PnL reflects the
  economics that actually make market making profitable.
- **Why it exists.** Without rebates, a tight-spread MM looks unprofitable on paper when it
  isn't. It also changes the optimal aggressiveness.
- **Code.** `position_keeper.h:88-95` (the rebate booking) — see M17.
- **Master it when you can…** explain maker vs. taker fees; explain why rebates change quoting
  incentives; read the sign convention (positive bps = rebate).
- **Your build / improvement.** `[v1.2]` Step 2.

---

## Tier 6 — Measurement, backtest, and honest results

You can't claim an improvement you didn't measure. This tier is how you proved the numbers in
`PERF.md` and `RESULTS.md`.

### M32 — Cycle-accurate instrumentation: RDTSC + TTT `[v1.0 base + your ch12 work]`
- **Idea.** `START_MEASURE`/`END_MEASURE` wrap a hot function and record the cycle delta via
  `rdtsc`; `TTT_MEASURE` stamps an absolute nanosecond timestamp at each queue/socket boundary.
  Together they let you reconstruct one trade's path to the cycle.
- **Why it exists.** Optimization without measurement is guessing. This is the ground truth
  behind every latency claim.
- **Code.** `common/perf_utils.h` (`START_MEASURE`, `END_MEASURE`, `END_MEASURE_HIST`,
  `TTT_MEASURE`, line ~15-40).
- **Master it when you can…** explain `rdtsc` vs. a clock syscall; explain why cycles need
  converting to ns; name three hot functions you instrumented.
- **Your build / improvement.** `[ch12]` you added `perf_utils.h` and instrumented both the
  exchange and the client hot paths (the `perf:` commits).

### M33 — Per-tag latency histograms `[v1.1/Day1]`
- **Idea.** Log2-bucketed histograms, one per instrumented tag, single-writer (no atomics on
  the bucket). Dumped to `latency_<client>_<tag>.hgrm` at shutdown and rendered to
  percentiles.
- **Why it exists.** A mean hides the tail (M01). Histograms give you p50/p99/p99.9 per
  function — the numbers that matter.
- **Code.** `common/latency_histogram.h`; dump via `TradeEngine::dumpLatencyHistograms`; render
  via `scripts/plot.py latency` → `docs/latency.png`.
- **Master it when you can…** explain why log2 buckets; explain why single-writer avoids
  atomics; read a `.hgrm` file.
- **Your build / improvement.** `[Day1]` yours — histograms + demo runner + percentile plot.

### M34 — The benchmark suite `[v1.0 + your perf days]`
- **Idea.** Four micro-benchmarks that isolate one optimization each: logger (block-copy
  **54×**), MemPool under NDEBUG (**25×**), the array-vs-hash LOB baseline, and scheduler
  jitter pinned-vs-unpinned (**11.6×**).
- **Why it exists.** Each is a defensible, reproducible claim — the evidence for the headline
  numbers.
- **Code.** `benchmarks/logger_benchmark.cpp`, `release_benchmark.cpp`, `hash_benchmark.cpp`,
  `jitter_benchmark.cpp`; results in `PERF.md`.
- **Master it when you can…** state what each benchmark isolates and its number; reproduce one
  from the Quickstart.
- **Your build / improvement.** `[ch12 + Day6]` — the jitter benchmark especially is yours.

### M35 — The backtest harness `[v1.1 + v1.2]` ★ the thing that makes claims travel
- **Idea.** Replays a real Binance tape and drives the **exact same** `MarketMaker` /
  `OrderManager` / `FeatureEngine` / `PositionKeeper` code as live — only Phases 3-4 (the TCP
  exchange round-trip) are swapped for an in-process, queue-aware fill simulator. So any
  improvement measured in backtest travels straight to the live path.
- **Why it exists.** It's how you A/B'd v1.1 vs. v1.2 honestly. Same code, same features, only
  the matcher differs.
- **Code.** `backtest/backtest_engine.{h,cpp}`, `backtest/binance_tape_reader.{h,cpp}`;
  `scripts/run_full_sweep.sh` (5 strategies × 3 symbols = 15 backtests).
- **Master it when you can…** explain which phases are shared with live and which are
  simulated; explain "queue-aware" fills; explain why sharing code is the whole point.
- **Your build / improvement.** `[v1.1]` harness + 4-strategy compare; `[v1.2]` unified sweep +
  per-feature decomposition + Binance ingest pipeline (`feat(scripts)`, `feat(backtest)`).

### M36 — Reading the results honestly `[v1.2]`
- **Idea.** The headline is −$45.17M → −$9.66M (78.6% portfolio loss reduction), but
  `RESULTS.md` decomposes it per technique and separates **algorithmic** improvement (better
  decisions) from **accounting** effects (e.g. the rebate credit). It also documents the
  logger-drain hang that cost 22 min/backtest.
- **Why it exists.** The discipline of *like-for-like* attribution is what makes the numbers
  credible — and is the most interview-defensible thing in the repo.
- **Code.** `RESULTS.md` (per-symbol breakdown, per-technique decomposition, §7 on the hang).
- **Master it when you can…** explain which gains are algorithmic vs. accounting; explain why a
  78.6% *loss reduction* on a still-negative book is honest framing, not spin.
- **Your build / improvement.** `[v1.2]` the analysis + the honesty are yours.

---

## Suggested order & progress checklist

Tiers are already in dependency order. The two ★ modules (M21 Avellaneda-Stoikov, M35
backtest) are the spine — everything else supports them.

```
Tier 0  Big picture .................. [ ] read README trade-flow, [ ] draw the round trip
Tier 1  Foundations .................. [ ] M01 [ ] M02 [ ] M03 [ ] M04 [ ] M05 [ ] M06 [ ] M07
Tier 2  Exchange ..................... [ ] M08 [ ] M09 [ ] M10 [ ] M11 [ ] M12
Tier 3  Trading client .............. [ ] M13 [ ] M14 [ ] M15 [ ] M16 [ ] M17 [ ] M18 [ ] M19
Tier 4  Strategy & quant math ....... [ ] M20 [ ] M21★ [ ] M22 [ ] M23 [ ] M24 [ ] M25
Tier 5  v1.2 defensive overlay ...... [ ] M26 [ ] M27 [ ] M28 [ ] M29 [ ] M30 [ ] M31
Tier 6  Measurement & results ....... [ ] M32 [ ] M33 [ ] M34 [ ] M35★ [ ] M36
```

**Next step:** pick a module and say "deep-dive M02" (or wherever you want to start). In a
deep-dive I'll derive it from first principles, read the actual code with you, quiz you on the
"Master it when you can…" questions, and trace it through one real trade.

---

## Reference docs in this repo

- `README.md` — architecture + full 5-phase trade-flow walkthrough.
- `STRATEGY.md` — v1.1 quoter math mapped to code.
- `PERF.md` — every latency percentile + benchmark, reproducible.
- `RESULTS.md` — v1.1 vs. v1.2 PnL, technique-by-technique, honest attribution.
- `notebooks/strategy_compare.html` — rendered comparison plots.
