# Binance Testnet adapter — scaffold

**Status: skeleton only.** This directory holds a non-functional drop-in
replacement for `Trading::OrderGateway` and `Trading::MarketDataConsumer`
that, when finished, would let the existing strategy stack (`TradeEngine` /
`MarketMaker` / `OrderManager` / `RiskManager`) trade on Binance Spot or
Futures Testnet via REST + WebSocket — **without touching any strategy code**.

Compiles as the `binance_adapter_skel` CMake target. Not linked into
`trading_main` by default.

## Why this is a scaffold, not the real thing

The whole reason the codebase has an `LFQueue<T>`-decoupled I/O edge is
exactly so that you can swap the *transport* (TCP loopback ↔ Binance WS)
without touching the *strategy*. This scaffold proves that contract and
marks every external integration point with `TODO(part2):` so the finishing
work is mechanical rather than architectural.

Counting `TODO(part2):` markers across the four files gives a rough estimate
of the remaining work:

```
grep -rn "TODO(part2):" .
```

## Architecture (when finished)

```
            ┌──────────────────────────────────────────┐
            │           TradeEngine (unchanged)        │
            └──────────────┬─────────────┬─────────────┘
                           │             │
              MD LFQ ◄──── │             │ ───► Request LFQ
                           │             │
      ┌────────────────────┴─┐         ┌─┴─────────────────────┐
      │ BinanceMdConsumer    │         │ BinanceWsGateway      │
      │ - subscribes to      │         │ - drains Request LFQ  │
      │   <sym>@bookTicker   │         │ - signs REST POSTs    │
      │   <sym>@trade        │         │ - reads user-data WS  │
      │ - translates JSON    │         │ - translates fills    │
      │   → MEMarketUpdate   │         │   → MEClientResponse  │
      └──────────┬───────────┘         └──────────▲────────────┘
                 │                                │
                 │       wss://testnet.binance.vision
                 └───────────────────►◄───────────┘
                       https://testnet.binance.vision/api/v3/order
```

## To finish in a weekend

Five concrete tasks, in order. The TODO markers in the source point at each.

### 1. WebSocket connect + reconnect (4 hrs)

Use **Boost.Beast** (header-only, ships with Boost). One `io_context` per
thread, `ssl::stream<tcp::socket>` upgraded with `boost::beast::websocket`.
Binance kills WS connections every 24 h — add reconnect-with-backoff and
re-subscribe on reconnect. PING/PONG is automatic via `control_callback`.

Files: `binance_md_consumer.cpp::run()`, `binance_ws_gateway.cpp::start()`

### 2. Signed REST POST (3 hrs)

Use **cpp-httplib** (header-only) or **libcurl**. Build a deterministic
query string, append `&timestamp=<ms>`, HMAC-SHA256 with `api_secret` via
OpenSSL's `HMAC()` API, append `&signature=<hex>`. Header:
`X-MBX-APIKEY: <api_key>`.

Files: `binance_ws_gateway.cpp::sendOrder()`

### 3. executionReport → MEClientResponse translation (3 hrs)

Use **nlohmann::json** (header-only). Map Binance's order-status enum to
the project's `ClientResponseType`:

| Binance `X`        | `MEClientResponse::type_` |
|--------------------|---------------------------|
| `NEW`              | `ACCEPTED`                |
| `PARTIALLY_FILLED` | `FILLED` (partial)        |
| `FILLED`           | `FILLED`                  |
| `CANCELED`         | `CANCELED`                |
| `REJECTED`         | `CANCEL_REJECTED`         |
| `EXPIRED`          | `CANCELED`                |

You'll need a (`newClientOrderId` → `OrderId`) lookup that `sendOrder()`
populates and `onExecutionReport()` reads.

Files: `binance_ws_gateway.cpp::onExecutionReport()`

### 4. bookTicker → MEMarketUpdate synthesis (3 hrs)

Binance's L1 stream only gives top-of-book updates; the MEOrderBook expects
`CANCEL` + `ADD` pairs to update the book. For each `bookTicker` frame,
synthesize:

1. `CANCEL` of the prior bid/ask order (if `last_top_[tid]` is valid)
2. `ADD` of the new bid/ask with `priority_ = 1`

This is identical to what the synthetic tape already does — `MarketMaker`
will be none the wiser.

Files: `binance_md_consumer.cpp::onBookTickerFrame()`

### 5. Rate-limit token bucket (2 hrs)

Binance Spot: **1200 weight/min** per IP, **50 orders / 10 s**. Replace the
`request_budget_` placeholder with a leaky bucket that decrements on every
POST and refills on a wall-clock interval. On 429/418, back off by
`Retry-After`.

Files: `binance_ws_gateway.cpp::run()`, `sendOrder()`

## Caveats — things that will go wrong when you go live

These are real risks from the literature; surface them in the showcase doc.

1. **Latency 1000× the synthetic tape.** Loopback is sub-µs; WS to Binance
   is 50–200 ms RTT. The OrderManager's hysteresis was tuned for fast
   feedback — re-tune `hyst_ticks` upward and slow the requote cadence.

2. **Rate limits will bite the maker strategy hard.** MarketMaker re-quotes
   aggressively. Without the token bucket you will get a 418 (IP ban) in
   minutes.

3. **AS adverse selection.** Synthetic-tape PnL ≠ live PnL — the maker
   gets toxic-flow-selected by informed takers. The Avellaneda-Stoikov
   paper assumes a Brownian mid; real flow has signed jumps. Add a
   **VPIN** toxicity gate (or a hard spread floor) before risking real
   money. See: *Improving Avellaneda-Stoikov with RL*, PLOS One, 2023.

4. **Fees flip the sign at retail tier.** Binance Spot maker fee is 10 bps
   (or 2 bps with BNB discount). The AS edge in the backtest is often
   smaller than that on illiquid pairs — verify the per-symbol fee-adjusted
   PnL before sizing up. See `scripts/analyze_pnl.py::MAKER_FEE`.

## Endpoint reference

- **Spot Testnet:**
  - REST   `https://testnet.binance.vision/api/v3`
  - WS     `wss://testnet.binance.vision/ws/<listenKey>` (user stream)
  - WS     `wss://testnet.binance.vision/stream?streams=<csv>` (combined market)
  - Docs   <https://testnet.binance.vision>

- **USDT-M Futures Testnet:**
  - REST   `https://testnet.binancefuture.com/fapi/v1`
  - WS     `wss://stream.binancefuture.com/ws`
  - Docs   <https://testnet.binancefuture.com>

## Why not just write the whole thing now?

Three reasons, in order of weight:

1. **The backtest already proves the strategy.** A weekend of WS plumbing
   that doesn't improve the alpha is a worse use of time than a notebook
   that quantifies adverse selection on the existing tape.
2. **Reviewer credibility comes from honest scaffolding, not vapor.**
   "Drop-in compiles, every TODO named" reads better on a resume than
   "live trading on day one" that turns out to be a one-symbol smoke test.
3. **The interesting research lives upstream of I/O.** VPIN toxicity gating,
   regime-switching σ, multi-level quoting — those are the next steps that
   actually move the needle.
