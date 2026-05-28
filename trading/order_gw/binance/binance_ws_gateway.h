#pragma once
//
// Binance Spot/Futures Testnet order gateway — SCAFFOLD ONLY.
//
// This is the Part 2 future-work hook: a drop-in replacement for the in-house
// Trading::OrderGateway (TCP, bespoke pragma-packed wire format) that talks
// to Binance Testnet over REST+WebSocket instead. The strategy layer
// (TradeEngine, MarketMaker, OrderManager, RiskManager) is unchanged because
// this class honors the same LFQueue contract:
//
//   - drains Exchange::ClientRequestLFQueue (outgoing orders)
//   - fills Exchange::ClientResponseLFQueue (incoming fills/cancels)
//
// To finish this adapter, complete every // TODO(part2): block below.
// Estimated effort: ~1-2 weekends. See trading/order_gw/binance/README.md.
//
// External dependencies (header-only / pkg-managed):
//   - Boost.Beast or websocketpp   — WebSocket client
//   - OpenSSL (libcrypto)          — HMAC-SHA256 request signing
//   - nlohmann/json (header-only)  — JSON encode/decode
//   - libcurl OR cpp-httplib       — HTTPS REST (signed POST /v1/order)
//
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "common/logging.h"
#include "common/macros.h"
#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/types.h"
#include "order_server/client_request.h"
#include "order_server/client_response.h"

namespace Trading::Binance {

  /// Endpoint config. Set from environment or CLI:
  ///   BINANCE_API_KEY    — testnet public key
  ///   BINANCE_API_SECRET — testnet secret (HMAC-SHA256 key)
  /// Defaults below target Binance Spot Testnet; futures testnet has
  /// different hosts (testnet.binancefuture.com).
  struct BinanceEndpoint {
    std::string rest_host   = "testnet.binance.vision";
    std::string ws_host     = "testnet.binance.vision";
    std::string user_stream = "/ws";   // joined with listenKey after /userDataStream
    int         rest_port   = 443;
    int         ws_port     = 443;
    std::string api_key;
    std::string api_secret;
  };

  /// Drop-in replacement for Trading::OrderGateway. Constructor signature
  /// kept LFQueue-compatible so trading_main.cpp wiring stays the same.
  class BinanceWsGateway {
  public:
    BinanceWsGateway(Common::ClientId client_id,
                     Exchange::ClientRequestLFQueue  *client_requests,
                     Exchange::ClientResponseLFQueue *client_responses,
                     BinanceEndpoint endpoint);

    ~BinanceWsGateway() {
      stop();
      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(2s);
    }

    auto start() -> void;
    auto stop()  -> void { run_ = false; }

    BinanceWsGateway()                                   = delete;
    BinanceWsGateway(const BinanceWsGateway &)           = delete;
    BinanceWsGateway(BinanceWsGateway &&)                = delete;
    BinanceWsGateway &operator=(const BinanceWsGateway &) = delete;
    BinanceWsGateway &operator=(BinanceWsGateway &&)      = delete;

  private:
    const Common::ClientId client_id_;
    BinanceEndpoint        endpoint_;

    [[maybe_unused]] Exchange::ClientRequestLFQueue  *outgoing_requests_  = nullptr;
    [[maybe_unused]] Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;

    std::atomic<bool> run_{false};
    std::string       time_str_;
    Common::Logger    logger_;

    // Token bucket for Binance rate limits (spot: 1200 req/min/IP, ~50 orders/10s).
    // TODO(part2): replace with a real leaky-bucket implementation that
    // tracks both order-rate and weight-based limits separately.
    std::atomic<int> request_budget_{0};

    /// Hot thread: drain outgoing_requests_ → translate → REST POST.
    auto run() noexcept -> void;

    /// Inbound thread: WS user-stream → translate executionReport → push to
    /// incoming_responses_.
    auto runUserStream() noexcept -> void;

    /// Translate an Exchange::MEClientRequest into a Binance REST order body.
    /// Signs with HMAC-SHA256 over (timestamp + query string).
    auto sendOrder(const Exchange::MEClientRequest &req) noexcept -> void;

    /// Parse a Binance executionReport JSON payload into MEClientResponse and
    /// push onto incoming_responses_.
    auto onExecutionReport(const std::string &json_payload) noexcept -> void;
  };

}  // namespace Trading::Binance
