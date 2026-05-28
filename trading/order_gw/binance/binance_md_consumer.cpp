//
// Binance Testnet market-data consumer — SCAFFOLD ONLY. Every external
// integration point is marked with `// TODO(part2):`.
//
#include "trading/order_gw/binance/binance_md_consumer.h"

namespace Trading::Binance {

  BinanceMdConsumer::BinanceMdConsumer(
      Common::ClientId client_id,
      Exchange::MEMarketUpdateLFQueue *market_updates,
      std::string ws_host,
      std::vector<std::string> symbols,
      SymbolTickerMap symbol_to_ticker)
    : client_id_(client_id),
      ws_host_(std::move(ws_host)),
      symbols_(std::move(symbols)),
      symbol_to_ticker_(std::move(symbol_to_ticker)),
      incoming_md_updates_(market_updates),
      logger_("trading_binance_md_consumer_" + std::to_string(client_id) + ".log") {
    logger_.log("%:% %() % BinanceMdConsumer constructed, %zu symbols\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), symbols_.size());
  }

  auto BinanceMdConsumer::start() -> void {
    run_ = true;
    ASSERT(Common::createAndStartThread(-1, "Trading/BinanceMdConsumer",
                                        [this]() { run(); }) != nullptr,
           "Failed to start BinanceMdConsumer thread.");
  }

  auto BinanceMdConsumer::run() noexcept -> void {
    // TODO(part2): boost::asio::io_context + boost::beast::websocket::stream
    //   over SSL. Connect to wss://<ws_host_>/stream?streams=<csv> where
    //   csv is the combined-stream URL built from `symbols_`, joining
    //   "<sym>@bookTicker/<sym>@trade" per symbol.
    //   Spot:    wss://testnet.binance.vision/stream
    //   Futures: wss://stream.binancefuture.com/stream
    // TODO(part2): handle 24-hour reconnect requirement — Binance kills WS
    //   connections every 24h. Add reconnect-with-backoff and re-subscribe.
    // TODO(part2): handle PING frames automatically (Boost.Beast can do this
    //   via control_callback). Binance pings every 3 min; pong within 10 min
    //   or you're disconnected.
    while (run_.load(std::memory_order_acquire)) {
      // TODO(part2): read one ws frame, decode JSON envelope:
      //   {"stream": "btcusdt@bookTicker", "data": {...}}
      // Dispatch on the stream suffix to onBookTickerFrame / onTradeFrame.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  auto BinanceMdConsumer::onBookTickerFrame(const std::string &json_payload) noexcept -> void {
    // TODO(part2): nlohmann::json::parse(json_payload). Read fields:
    //   "s"  symbol
    //   "b"  best bid price (string)  /  "B"  best bid qty
    //   "a"  best ask price (string)  /  "A"  best ask qty
    //   "T"  transaction time (ms)
    //
    // Convert symbol -> ticker_id via symbol_to_ticker_, convert price to
    // ticks using the per-symbol tick_size (configured upstream), then
    // synthesize the MEMarketUpdate sequence MEOrderBook expects.
    //
    // Because we only have L1 (best bid/ask), each book-top change becomes:
    //   1. CANCEL the prior bid/ask order (if last_top_[tid] is valid)
    //   2. ADD the new bid/ask with priority_ = 1 (top of queue)
    // This loses queue depth at non-top levels but matches the synthetic
    // tape format the backtest already uses.
    //
    // TODO(part2): push each resulting MEMarketUpdate via:
    //   auto next = incoming_md_updates_->getNextToWriteTo();
    //   *next = update;
    //   incoming_md_updates_->updateWriteIndex();
    (void)json_payload;
  }

  auto BinanceMdConsumer::onTradeFrame(const std::string &json_payload) noexcept -> void {
    // TODO(part2): parse fields:
    //   "s"  symbol
    //   "p"  price (string)
    //   "q"  quantity (string)
    //   "m"  is buyer market maker
    //   "T"  trade time (ms)
    // Synthesize an MEMarketUpdate of type_=TRADE with the correct side
    // (BUY if !m else SELL) and push onto incoming_md_updates_.
    (void)json_payload;
  }

}  // namespace Trading::Binance
