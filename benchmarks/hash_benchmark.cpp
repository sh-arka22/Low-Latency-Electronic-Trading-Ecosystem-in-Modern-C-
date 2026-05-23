// Benchmark for the array-backed Exchange::MEOrderBook.
//
// Chapter 12's instruction also defines an UnorderedMapMEOrderBook variant for
// A/B comparison; per project decision we keep only the array-based design (it
// wins by ~6-7% on bounded integer key spaces) and measure it alone here as a
// regression baseline.

#include <cstdlib>
#include <iostream>
#include <vector>

#include "common/logging.h"
#include "common/perf_utils.h"
#include "common/types.h"
#include "matcher/matching_engine.h"
#include "matcher/me_order_book.h"
#include "order_server/client_request.h"

template <typename Book>
static size_t benchmarkHashMap(Book *book,
                               const std::vector<Exchange::MEClientRequest> &reqs) {
  size_t total_rdtsc = 0;
  for (const auto &req : reqs) {
    switch (req.type_) {
      case Exchange::ClientRequestType::NEW: {
        const auto start = Common::rdtsc();
        book->add(req.client_id_, req.order_id_, req.ticker_id_,
                  req.side_, req.price_, req.qty_);
        total_rdtsc += (Common::rdtsc() - start);
      } break;
      case Exchange::ClientRequestType::CANCEL: {
        const auto start = Common::rdtsc();
        book->cancel(req.client_id_, req.order_id_, req.ticker_id_);
        total_rdtsc += (Common::rdtsc() - start);
      } break;
      default: break;
    }
  }
  return total_rdtsc / reqs.size();
}

int main(int, char **) {
  srand(0);

  Common::Logger logger("hash_benchmark.log");

  Exchange::ClientRequestLFQueue  client_requests(Common::ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(Common::ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(Common::ME_MAX_MARKET_UPDATES);

  auto *matching_engine = new Exchange::MatchingEngine(
      &client_requests, &client_responses, &market_updates);

  constexpr size_t loop_count = 100000;
  std::vector<Exchange::MEClientRequest> reqs;
  reqs.reserve(loop_count * 2);

  Common::OrderId order_id   = 1000;
  Common::Price   base_price = static_cast<Common::Price>((rand() % 100) + 100);

  while (reqs.size() < loop_count * 2) {
    const Common::Price price = base_price + (rand() % 10) + 1;
    const Common::Qty   qty   = static_cast<Common::Qty>(1 + (rand() % 100) + 1);
    const Common::Side  side  = (rand() % 2 ? Common::Side::BUY : Common::Side::SELL);

    Exchange::MEClientRequest neu{
        Exchange::ClientRequestType::NEW,
        /*client_id*/ 0, /*ticker_id*/ 0, order_id++, side, price, qty};
    reqs.push_back(neu);

    auto cxl = reqs[rand() % reqs.size()];
    cxl.type_ = Exchange::ClientRequestType::CANCEL;
    reqs.push_back(cxl);
  }

  auto *book = new Exchange::MEOrderBook(/*ticker*/ 0, &logger, matching_engine);
  const auto cycles = benchmarkHashMap(book, reqs);
  std::cout << "ARRAY HASHMAP " << cycles << " CLOCK CYCLES PER OPERATION." << std::endl;

  // Intentionally leak: matching_engine's destructor stops a thread that we
  // never started, and book deletion triggers logger writes after the logger
  // destructor would have begun. Process exit cleans up.
  (void)book;
  (void)matching_engine;
  return EXIT_SUCCESS;
}
