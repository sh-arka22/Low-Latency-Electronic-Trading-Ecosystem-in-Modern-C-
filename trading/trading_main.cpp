#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "common/logging.h"
#include "common/types.h"
#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"

// Globals so the SIGINT handler can tear them down in reverse order.
Common::Logger              *logger               = nullptr;
Trading::TradeEngine        *trade_engine         = nullptr;
Trading::MarketDataConsumer *market_data_consumer = nullptr;
Trading::OrderGateway       *order_gateway        = nullptr;

void signal_handler(int) {
  using namespace std::literals::chrono_literals;

  // Graceful shutdown: stop() drains the LFQueues and logs the POSITIONS dump
  // before run_ is set to false. Order matters — trade_engine must finish
  // first so the position dump is written.
  if (trade_engine)         trade_engine->stop();
  if (market_data_consumer) market_data_consumer->stop();
  if (order_gateway)        order_gateway->stop();

  std::this_thread::sleep_for(2s);

  delete trade_engine;         trade_engine         = nullptr;
  delete market_data_consumer; market_data_consumer = nullptr;
  delete order_gateway;        order_gateway        = nullptr;
  delete logger;               logger               = nullptr;

  std::this_thread::sleep_for(1s);
  std::exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "Usage: %s CLIENT_ID ALGO_TYPE "
            "[clip threshold max_order_size max_position max_loss "
            "use_as gamma kappa session_hours use_ofi beta_ofi hyst_ticks "
            "use_adaptive_clip sigma_ref] * ME_MAX_TICKERS\n"
            "  v1.0 compat: 5 fields per ticker (clip..max_loss). v1.1 AS+OFI:\n"
            "  append 8 more (use_as gamma kappa session_hours use_ofi beta_ofi\n"
            "  hyst_ticks use_adaptive_clip sigma_ref) for full inventory-aware mode.\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  std::signal(SIGINT, signal_handler);

  const Common::ClientId client_id = atoi(argv[1]);
  srand(client_id);
  const auto algo_type = stringToAlgoType(argv[2]);

  // v1.0 path = 5 fields per ticker. v1.1 path appends 9 more fields, so the
  // per-ticker stride is either 5 or 14. Detect from how many args were given.
  const int kCfgArgs   = argc - 3;
  const int kV11Stride = 14;
  const int kV10Stride = 5;
  const int stride = (kCfgArgs > 0 && kCfgArgs % kV11Stride == 0)
                   ? kV11Stride : kV10Stride;

  TradeEngineCfgHashMap ticker_cfg;
  size_t next_ticker_id = 0;
  for (int i = 3; i < argc && next_ticker_id < ME_MAX_TICKERS;
       i += stride, ++next_ticker_id) {
    TradeEngineCfg cfg{};
    cfg.clip_      = static_cast<Qty>(std::atoi(argv[i]));
    cfg.threshold_ = std::atof(argv[i + 1]);
    cfg.risk_cfg_  = {
        static_cast<Qty>(std::atoi(argv[i + 2])),
        static_cast<Qty>(std::atoi(argv[i + 3])),
        std::atof(argv[i + 4])
    };
    if (stride == kV11Stride) {
      cfg.use_as_            = std::atoi(argv[i + 5]) != 0;
      cfg.gamma_             = std::atof(argv[i + 6]);
      cfg.kappa_             = std::atof(argv[i + 7]);
      cfg.session_hours_     = std::atof(argv[i + 8]);
      cfg.use_ofi_           = std::atoi(argv[i + 9]) != 0;
      cfg.beta_ofi_          = std::atof(argv[i + 10]);
      cfg.hysteresis_ticks_  = static_cast<Price>(std::atoi(argv[i + 11]));
      cfg.use_adaptive_clip_ = std::atoi(argv[i + 12]) != 0;
      cfg.sigma_ref_         = std::atof(argv[i + 13]);
    }
    ticker_cfg.at(next_ticker_id) = cfg;
  }

  logger = new Common::Logger("trading_main_" + std::to_string(client_id) + ".log");
  const int sleep_time = 20 * 1000;  // 20 ms in microseconds for RANDOM
  Exchange::ClientRequestLFQueue  client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);
  std::string time_str;

  // 1. TradeEngine
  logger->log("%:% %() % Starting Trade Engine...\n",
              __FILE__, __LINE__, __FUNCTION__,
              Common::getCurrentTimeStr(&time_str));
  trade_engine = new Trading::TradeEngine(client_id, algo_type, ticker_cfg,
                                          &client_requests, &client_responses,
                                          &market_updates);
  trade_engine->start();

  // 2. OrderGateway — TCP into exchange OrderServer on lo0:12345
  const std::string order_gw_ip    = "127.0.0.1";
  const std::string order_gw_iface = "lo0";
  const int         order_gw_port  = 12345;
  logger->log("%:% %() % Starting Order Gateway...\n",
              __FILE__, __LINE__, __FUNCTION__,
              Common::getCurrentTimeStr(&time_str));
  order_gateway = new Trading::OrderGateway(client_id, &client_requests,
                                            &client_responses, order_gw_ip,
                                            order_gw_iface, order_gw_port);
  order_gateway->start();

  // 3. MarketDataConsumer — UDP multicast snapshot+incremental
  const std::string mkt_data_iface  = "lo0";
  const std::string snapshot_ip     = "233.252.14.1";
  const int         snapshot_port   = 20000;
  const std::string incremental_ip  = "233.252.14.3";
  const int         incremental_port = 20001;
  logger->log("%:% %() % Starting Market Data Consumer...\n",
              __FILE__, __LINE__, __FUNCTION__,
              Common::getCurrentTimeStr(&time_str));
  market_data_consumer = new Trading::MarketDataConsumer(
      client_id, &market_updates, mkt_data_iface,
      snapshot_ip, snapshot_port, incremental_ip, incremental_port);
  market_data_consumer->start();

  // 4. Warm-up: let all threads reach steady state before trading.
  usleep(10 * 1000 * 1000);  // 10 seconds

  trade_engine->initLastEventTime();

  // 5. RANDOM strategy runs its trading loop directly from main().
  if (algo_type == AlgoType::RANDOM) {
    Common::OrderId order_id = client_id * 1000;
    std::vector<Exchange::MEClientRequest> client_requests_vec;
    std::array<Price, ME_MAX_TICKERS> ticker_base_price;
    for (size_t i = 0; i < ME_MAX_TICKERS; ++i)
      ticker_base_price[i] = (rand() % 100) + 100;

    trade_engine->initLastEventTime();

    for (size_t i = 0; i < 10000; ++i) {
      const Common::TickerId ticker_id = rand() % Common::ME_MAX_TICKERS;
      const Price price = ticker_base_price[ticker_id] + (rand() % 10) + 1;
      const Qty   qty   = 1 + (rand() % 100) + 1;
      const Side  side  = (rand() % 2 ? Common::Side::BUY : Common::Side::SELL);

      Exchange::MEClientRequest new_request{
          Exchange::ClientRequestType::NEW,
          client_id, ticker_id, order_id++, side, price, qty};
      trade_engine->sendClientRequest(&new_request);
      usleep(sleep_time);
      client_requests_vec.push_back(new_request);

      const auto cxl_index   = rand() % client_requests_vec.size();
      auto       cxl_request = client_requests_vec[cxl_index];
      cxl_request.type_      = Exchange::ClientRequestType::CANCEL;
      trade_engine->sendClientRequest(&cxl_request);
      usleep(sleep_time);
    }
  }

  // 6. Idle until the market has been quiet for 60 seconds, then shut down.
  while (trade_engine->silentSeconds() < 60) {
    logger->log("%:% %() % Waiting till no activity, been silent for % seconds...\n",
                __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str),
                trade_engine->silentSeconds());
    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(10s);
  }

  trade_engine->stop();
  market_data_consumer->stop();
  order_gateway->stop();

  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);

  delete logger;               logger               = nullptr;
  delete trade_engine;         trade_engine         = nullptr;
  delete market_data_consumer; market_data_consumer = nullptr;
  delete order_gateway;        order_gateway        = nullptr;

  std::this_thread::sleep_for(10s);
  exit(EXIT_SUCCESS);
}
