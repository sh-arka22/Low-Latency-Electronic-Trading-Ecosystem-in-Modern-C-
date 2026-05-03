# Electronic Trading Ecosystem

A high-performance, low-latency electronic trading system written in C++20. Built from first principles with zero heap allocation on hot paths, lock-free inter-thread communication, and strict separation between exchange-side and participant-side components.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        EXCHANGE SIDE                            │
│                                                                 │
│  ┌──────────────────┐   LFQueue    ┌────────────────────────┐  │
│  │  Order Gateway   │ ──────────► │   Matching Engine      │  │
│  │  Server (TCP)    │ ◄────────── │   (LOB + Matching)     │  │
│  └──────────────────┘   LFQueue    └──────────┬─────────────┘  │
│                                               │ LFQueue        │
│                                    ┌──────────▼─────────────┐  │
│                                    │  Market Data Publisher │  │
│                                    │  (UDP Multicast)       │  │
│                                    └────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
           TCP ▲                          UDP Multicast ▼
┌─────────────────────────────────────────────────────────────────┐
│                    MARKET PARTICIPANT SIDE                      │
│                                                                 │
│  ┌──────────────────┐              ┌────────────────────────┐  │
│  │  Order Gateway   │              │  Market Data Consumer  │  │
│  │  Client (TCP)    │              │  (UDP subscriber)      │  │
│  └────────┬─────────┘              └──────────┬─────────────┘  │
│           │ LFQueue                           │ LFQueue        │
│           └──────────────┬────────────────────┘                │
│                          ▼                                      │
│              ┌───────────────────────┐                         │
│              │   Trading Engine      │                         │
│              │  ┌─────────────────┐  │                         │
│              │  │ Client Order    │  │                         │
│              │  │    Book         │  │                         │
│              │  │ Feature Engine  │  │                         │
│              │  │ Trading Strategy│  │                         │
│              │  │ Order Manager   │  │                         │
│              │  │  Risk Manager   │  │                         │
│              │  └─────────────────┘  │                         │
│              └───────────────────────┘                         │
└─────────────────────────────────────────────────────────────────┘
```

### Threading Model

Each major component runs on its own dedicated OS thread. Cross-thread communication is **exclusively** via `LFQueue<T>` — no mutexes, no condition variables, no shared mutable state.

| Thread | Component | Communicates via |
|--------|-----------|-----------------|
| Exchange/MatchingEngine | Core LOB matching | 3 LFQueues (in: requests, out: responses + market updates) |
| Common/Logger | Async log flusher | LFQueue of `LogElement` |

---

## Components

### `common/` — Low-Latency Building Blocks

| File | What it provides |
|------|-----------------|
| `macros.h` | `LIKELY`, `UNLIKELY`, `ASSERT`, `FATAL` — branch-hint and crash macros |
| `types.h` | Named typedefs (`OrderId`, `Price`, `Qty`, `Side`, …) + `_INVALID` sentinels |
| `mem_pool.h` | `MemPool<T>` — fixed-capacity slab allocator, O(1) alloc/dealloc, zero heap on hot path |
| `lf_queue.h` | `LFQueue<T>` — lock-free SPSC ring buffer backed by pre-allocated storage |
| `thread_utils.h` | `createAndStartThread()` with CPU affinity (Linux); macOS no-op fallback |
| `time_utils.h` | `getCurrentTimeStr()`, nanosecond clock utilities |
| `logging.h/.cpp` | `Logger` — async, lock-free logger; I/O only on the dedicated flush thread |

### `exchange/order_server/` — Wire Protocol (Exchange ↔ Client)

| File | What it provides |
|------|-----------------|
| `client_request.h` | `MEClientRequest` — NEW/CANCEL packed struct; `ClientRequestLFQueue` typedef |
| `client_response.h` | `MEClientResponse` — ACCEPTED/CANCELED/FILLED/CANCEL_REJECTED; `ClientResponseLFQueue` typedef |

All structs use `#pragma pack(1)` so they can be sent directly over the network as raw bytes.

### `exchange/market_data/` — Public Market Feed

| File | What it provides |
|------|-----------------|
| `market_update.h` | `MEMarketUpdate` — ADD/MODIFY/CANCEL/TRADE packed struct; `MEMarketUpdateLFQueue` typedef |

### `exchange/matcher/` — Limit Order Book & Matching Engine

| File | What it provides |
|------|-----------------|
| `me_order.h/.cpp` | `MEOrder` (circular doubly-linked within price level), `MEOrdersAtPrice` (price level node), flat-array hash maps |
| `me_order_book.h/.cpp` | `MEOrderBook` — full price-time priority LOB: add, cancel, match, pool-backed |
| `matching_engine.h/.cpp` | `MatchingEngine` — owns one `MEOrderBook` per ticker, single-thread hot loop |

#### Order Book Layout

```
bids_by_price_ ──► [Price=1090] ◄──► [Price=1080] ◄──► [Price=1070]  (circular)
                        │
                  first_me_order_
                        │
               ┌────────▼─────────────────────────────────────────┐
               │ MEOrder{prio=1, qty=20, client=A} ◄──► ...        │  (circular, FIFO)
               └──────────────────────────────────────────────────┘
```

- **Bids**: sorted highest → lowest  
- **Asks**: sorted lowest → highest  
- All `MEOrder` and `MEOrdersAtPrice` objects live in `MemPool<T>` — no `new`/`delete` during matching

#### Matching Flow

```
add() called
 ├─► emit ACCEPTED response
 ├─► checkForMatch() — greedily walk opposite side while prices cross
 │    └─► match() per fill:
 │         ├─► FILLED response (aggressor)
 │         ├─► FILLED response (passive)
 │         ├─► TRADE market update (public)
 │         └─► CANCEL or MODIFY market update (passive order state)
 └─► if leaves_qty > 0: rest on book + emit ADD market update
```

---

## Design Invariants

These are never violated anywhere in the codebase:

| Invariant | Rationale |
|-----------|-----------|
| No `new`/`delete` on hot paths | `MemPool<T>` gives O(1) alloc with zero OS interaction |
| No `std::unordered_map` | Flat `std::array` with modulo indexing — cache-friendly, no rehashing |
| No mutex/condvar between threads | `LFQueue<T>` is the only cross-thread channel |
| All network structs are `#pragma pack(1)` | Zero-copy serialization — struct IS the wire format |
| `volatile bool run_` for the run flag | Visible across threads without a fence/mutex |
| Every type has an `_INVALID` sentinel | Caught early in logging and assertion paths |
| Deleted copy/move constructors everywhere | No accidental copies of engine-owned objects |

---

## Build

### Requirements

- CMake ≥ 3.20
- Ninja (or Make)
- GCC ≥ 11 or Clang ≥ 14 with C++20 support

### Steps

```bash
bash build.sh
```

Or manually:

```bash
mkdir -p cmake-build-release && cd cmake-build-release
cmake -DCMAKE_BUILD_TYPE=Release -G Ninja ..
ninja
```

### Run

```bash
./cmake-build-release/exchange_main
```

Expected output on startup:
```
Set core affinity for Common/Logger exchange_main.log ... to -1
Set core affinity for Common/Logger exchange_matching_engine.log ... to -1
Set core affinity for Exchange/MatchingEngine ... to -1
```

Stop with `Ctrl+C` — triggers a graceful SIGINT shutdown.

### macOS Note

Thread CPU affinity (`pthread_setaffinity_np`) is Linux-only. On macOS, `setThreadCore()` is a no-op and returns `true` — all functionality works, just without core pinning. Run on Linux for production-accurate latency profiling.

---

## Current Status

- [x] Common building blocks — memory pool, lock-free queue, async logger, threading, sockets
- [x] Exchange wire protocol — client request/response and market update structs
- [x] Limit order book — full add/cancel/match with price-time priority
- [x] Matching engine — single-thread run loop, dispatches to per-ticker order books
- [x] Exchange entry point — SIGINT-safe startup/shutdown
- [ ] Order Gateway Server — TCP server + FIFO sequencer
- [ ] Market Data Publisher — UDP multicast incremental feed
- [ ] Snapshot Synthesizer — periodic full-book UDP snapshot
- [ ] Market Data Consumer — client-side incremental + snapshot recovery
- [ ] Order Gateway Client — client-side TCP order sender
- [ ] Trading Engine — market data → signal → order lifecycle
- [ ] Feature Engine — fair price, trade imbalance signals
- [ ] Risk Manager — pre-trade position and loss checks
- [ ] Market Maker strategy
- [ ] Liquidity Taker strategy
- [ ] Performance instrumentation — RDTSC latency probes
- [ ] Optimized logger and memory pool

---

## Repository Structure

```
electronic_trading_ecosystem/
├── CMakeLists.txt
├── build.sh
├── common/
│   ├── macros.h
│   ├── types.h
│   ├── mem_pool.h
│   ├── lf_queue.h
│   ├── thread_utils.h
│   ├── time_utils.h
│   └── logging.h / logging.cpp
└── exchange/
    ├── exchange_main.cpp
    ├── order_server/
    │   ├── client_request.h
    │   └── client_response.h
    ├── market_data/
    │   └── market_update.h
    └── matcher/
        ├── me_order.h / me_order.cpp
        ├── me_order_book.h / me_order_book.cpp
        └── matching_engine.h / matching_engine.cpp
```
