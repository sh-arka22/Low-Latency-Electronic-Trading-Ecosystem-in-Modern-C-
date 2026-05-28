//
// Binance Testnet order gateway — SCAFFOLD ONLY. Every method body below is
// a placeholder; every external integration point is marked with
// `// TODO(part2):` plus the library and 1-line description.
//
// This compiles cleanly as a standalone target (binance_adapter_skel) but is
// NOT linked into trading_main — see CMakeLists.txt.
//
#include "trading/order_gw/binance/binance_ws_gateway.h"

namespace Trading::Binance {

  BinanceWsGateway::BinanceWsGateway(
      Common::ClientId client_id,
      Exchange::ClientRequestLFQueue  *client_requests,
      Exchange::ClientResponseLFQueue *client_responses,
      BinanceEndpoint endpoint)
    : client_id_(client_id),
      endpoint_(std::move(endpoint)),
      outgoing_requests_(client_requests),
      incoming_responses_(client_responses),
      logger_("trading_binance_ws_gateway_" + std::to_string(client_id) + ".log") {
    logger_.log("%:% %() % BinanceWsGateway constructed for client_id:%\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), client_id_);
  }

  auto BinanceWsGateway::start() -> void {
    run_ = true;

    // TODO(part2): boost::asio::io_context + boost::beast::ssl_stream:
    //   open HTTPS connection to endpoint_.rest_host:443, verify SSL cert
    //   against system trust store. Persist for the run() thread.

    // TODO(part2): POST /api/v3/userDataStream with X-MBX-APIKEY header
    //   to obtain listenKey. Schedule a 30-min keep-alive PUT to extend it.

    // TODO(part2): open WebSocket to endpoint_.ws_host:443/ws/<listenKey>
    //   for the user data stream (executionReport messages). Spawn the
    //   runUserStream() thread that pumps WS frames into onExecutionReport().

    ASSERT(Common::createAndStartThread(-1, "Trading/BinanceWsGateway/REST",
                                        [this]() { run(); }) != nullptr,
           "Failed to start BinanceWsGateway REST thread.");
    ASSERT(Common::createAndStartThread(-1, "Trading/BinanceWsGateway/UserStream",
                                        [this]() { runUserStream(); }) != nullptr,
           "Failed to start BinanceWsGateway user-stream thread.");
  }

  auto BinanceWsGateway::run() noexcept -> void {
    logger_.log("%:% %() % gateway REST loop started\n", __FILE__, __LINE__,
                __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_.load(std::memory_order_acquire)) {
      // Drain outgoing requests from the trade engine.
      auto req = outgoing_requests_->getNextToRead();
      if (LIKELY(req)) {
        // TODO(part2): token-bucket gate — if request_budget_ == 0, sleep
        // until the next refill (spot tier: 50 orders / 10 s).
        sendOrder(*req);
        outgoing_requests_->updateReadIndex();
      }
    }
  }

  auto BinanceWsGateway::runUserStream() noexcept -> void {
    logger_.log("%:% %() % gateway user-stream loop started\n", __FILE__,
                __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while (run_.load(std::memory_order_acquire)) {
      // TODO(part2): boost::beast::websocket::stream::read into a flat_buffer,
      // decode UTF-8, dispatch on JSON "e" field == "executionReport" to
      // onExecutionReport(). Handle "outboundAccountPosition" for sanity.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  auto BinanceWsGateway::sendOrder(const Exchange::MEClientRequest &req) noexcept -> void {
    // TODO(part2): translate MEClientRequest -> Binance REST POST body.
    //   - req.type_ == NEW    -> POST /api/v3/order with type=LIMIT, GTC,
    //                            timeInForce=GTC, price=req.price_,
    //                            quantity=req.qty_,
    //                            side=BUY/SELL from req.side_,
    //                            newClientOrderId=<client_id_>_<req.order_id_>
    //   - req.type_ == CANCEL -> DELETE /api/v3/order with origClientOrderId.
    // TODO(part2): build the query string in deterministic key=value order,
    //   append &timestamp=<ms>, then HMAC-SHA256-sign with api_secret via
    //   OpenSSL's HMAC()/EVP_MAC API. Append &signature=<hex>.
    // TODO(part2): set header X-MBX-APIKEY: <api_key>. cpp-httplib::Client
    //   or libcurl with verifypeer=on.
    // TODO(part2): on 200 OK, parse the orderId from the JSON response and
    //   stash a (newClientOrderId -> req.order_id_) map for the user stream
    //   to resolve back when executionReport arrives.
    // TODO(part2): on 429 (rate limit) or 418 (IP ban), back off using the
    //   Retry-After header. Decrement request_budget_ atomically.
    (void)req;
  }

  auto BinanceWsGateway::onExecutionReport(const std::string &json_payload) noexcept -> void {
    // TODO(part2): nlohmann::json::parse(json_payload) and read fields:
    //   "c"  client_order_id   -> map back to original req.order_id_
    //   "X"  current order status NEW/FILLED/PARTIALLY_FILLED/CANCELED/REJECTED
    //   "l"  last filled qty
    //   "L"  last filled price
    //   "z"  cumulative filled qty
    //   "S"  side BUY/SELL
    //   "T"  transaction time (ms)
    // TODO(part2): construct an MEClientResponse with the appropriate
    //   type_ (ACCEPTED / FILLED / CANCELED / CANCEL_REJECTED) and push
    //   onto incoming_responses_ via getNextToWriteTo() / updateWriteIndex().
    (void)json_payload;
  }

}  // namespace Trading::Binance
