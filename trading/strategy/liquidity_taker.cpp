#include "strategy/liquidity_taker.h"
#include "strategy/trade_engine.h"

namespace Trading {

  LiquidityTaker::LiquidityTaker(Common::Logger *logger,
                                  TradeEngine *trade_engine,
                                  FeatureEngine *feature_engine,
                                  OrderManager *order_manager,
                                  const TradeEngineCfgHashMap &ticker_cfg)
      : feature_engine_(feature_engine),
        order_manager_(order_manager),
        logger_(logger),
        ticker_cfg_(ticker_cfg) {

    trade_engine->algoOnOrderBookUpdate_ =
        [this](auto ticker_id, auto price, auto side, auto book) {
          onOrderBookUpdate(ticker_id, price, side, book);
        };
    trade_engine->algoOnTradeUpdate_ =
        [this](auto market_update, auto book) {
          onTradeUpdate(market_update, book);
        };
    trade_engine->algoOnOrderUpdate_ =
        [this](auto client_response) {
          onOrderUpdate(client_response);
        };
  }

  auto LiquidityTaker::onTradeUpdate(const Exchange::MEMarketUpdate *market_update,
                                      MarketOrderBook *book) noexcept -> void {
    logger_->log("%:% %() % %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 market_update->toString().c_str());

    const auto bbo           = book->getBBO();
    const auto agg_qty_ratio = feature_engine_->getAggTradeQtyRatio();

    if (LIKELY(bbo->bid_price_ != Price_INVALID &&
               bbo->ask_price_ != Price_INVALID &&
               agg_qty_ratio   != Feature_INVALID)) {

      logger_->log("%:% %() % % agg-qty-ratio:%\n",
                   __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_),
                   bbo->toString().c_str(), agg_qty_ratio);

      const auto clip      = ticker_cfg_.at(market_update->ticker_id_).clip_;
      const auto threshold = ticker_cfg_.at(market_update->ticker_id_).threshold_;

      if (agg_qty_ratio >= threshold) {
        START_MEASURE(Trading_OrderManager_moveOrders);
        if (market_update->side_ == Side::BUY)
          order_manager_->moveOrders(market_update->ticker_id_,
                                     bbo->ask_price_, Price_INVALID, clip);
        else
          order_manager_->moveOrders(market_update->ticker_id_,
                                     Price_INVALID, bbo->bid_price_, clip);
        END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
      }
    }
  }

  auto LiquidityTaker::onOrderBookUpdate(TickerId ticker_id, Price price,
                                          Side side, MarketOrderBook *) noexcept -> void {
    logger_->log("%:% %() % ticker:% price:% side:%\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 ticker_id, Common::priceToString(price).c_str(),
                 Common::sideToString(side).c_str());
  }

  auto LiquidityTaker::onOrderUpdate(const Exchange::MEClientResponse *client_response)
      noexcept -> void {
    logger_->log("%:% %() % %\n",
                 __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(&time_str_),
                 client_response->toString().c_str());

    START_MEASURE(Trading_OrderManager_onOrderUpdate);
    order_manager_->onOrderUpdate(client_response);
    END_MEASURE(Trading_OrderManager_onOrderUpdate, (*logger_));
  }

}
