#pragma once
//
// Binance Spot/Futures Testnet market-data consumer — SCAFFOLD ONLY.
//
// Drop-in replacement for Trading::MarketDataConsumer (UDP multicast). Honors
// the same LFQueue contract — writes MEMarketUpdate events onto the queue
// TradeEngine drains — so the strategy layer is unchanged.
//
// Subscribes to two Binance WebSocket streams per symbol:
//   wss://<host>/stream?streams=<sym>@bookTicker/<sym>@trade
// and translates each frame into MEMarketUpdate.
//
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging.h"
#include "common/macros.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "market_data/market_update.h"

namespace Trading::Binance {

  /// Maps Binance symbol strings ("BTCUSDT") -> ticker_id ints used by the
  /// strategy layer. Built once at startup from the strategy config.
  using SymbolTickerMap = std::unordered_map<std::string, Common::TickerId>;

  class BinanceMdConsumer {
  public:
    BinanceMdConsumer(Common::ClientId client_id,
                      Exchange::MEMarketUpdateLFQueue *market_updates,
                      std::string ws_host,
                      std::vector<std::string> symbols,
                      SymbolTickerMap symbol_to_ticker);

    ~BinanceMdConsumer() {
      stop();
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(2s);
    }

    auto start() -> void;
    auto stop()  -> void { run_ = false; }

    BinanceMdConsumer()                                    = delete;
    BinanceMdConsumer(const BinanceMdConsumer &)            = delete;
    BinanceMdConsumer(BinanceMdConsumer &&)                 = delete;
    BinanceMdConsumer &operator=(const BinanceMdConsumer &) = delete;
    BinanceMdConsumer &operator=(BinanceMdConsumer &&)      = delete;

  private:
    [[maybe_unused]] const Common::ClientId   client_id_;
    std::string                               ws_host_;
    std::vector<std::string>                  symbols_;
    SymbolTickerMap                           symbol_to_ticker_;

    [[maybe_unused]] Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    std::atomic<bool> run_{false};
    std::string       time_str_;
    Common::Logger    logger_;

    // Last seen best bid/ask per ticker — needed to synthesize the
    // CLEAR/MODIFY/ADD sequence MEOrderBook expects when only L1 is available.
    struct BookTop {
      Common::Price  bid_price = Common::Price_INVALID;
      Common::Qty    bid_qty   = Common::Qty_INVALID;
      Common::Price  ask_price = Common::Price_INVALID;
      Common::Qty    ask_qty   = Common::Qty_INVALID;
    };
    std::unordered_map<Common::TickerId, BookTop> last_top_;

    auto run() noexcept -> void;

    auto onBookTickerFrame(const std::string &json_payload) noexcept -> void;
    auto onTradeFrame     (const std::string &json_payload) noexcept -> void;
  };

}  // namespace Trading::Binance
