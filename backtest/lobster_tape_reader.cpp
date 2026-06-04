#include "backtest/lobster_tape_reader.h"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Backtest {

  using Common::Nanos;
  using Common::OrderId;
  using Common::Priority;
  using Common::Qty;
  using Common::Price;
  using Common::Side;
  using Common::OrderId_INVALID;
  using Common::Priority_INVALID;
  using Exchange::MEMarketUpdate;
  using Exchange::MarketUpdateType;

  namespace {
    // Deterministic per-(side,price) OID: bids use the cent-price directly
    // (< 131072), asks are offset clear of the bid range and of ME_MAX_ORDER_IDS.
    constexpr OrderId kAskOidOffset = 600'000;
    // LOBSTER empty-level price sentinels are +-9999999999.
    constexpr int64_t kSentinel = 9'999'999'990;

    std::string deriveOrderbookPath(const std::string &msg) {
      std::string s = msg;
      const auto pos = s.find("message");
      if (pos != std::string::npos) s.replace(pos, 7, "orderbook");
      return s;
    }
  }

  LobsterReader::LobsterReader(Config cfg) : cfg_(std::move(cfg)) {
    msg_in_.open(cfg_.message_path);
    if (!msg_in_) throw std::runtime_error("LobsterReader: cannot open " + cfg_.message_path);
    const auto ob_path = deriveOrderbookPath(cfg_.message_path);
    ob_in_.open(ob_path);
    if (!ob_in_) throw std::runtime_error("LobsterReader: cannot open " + ob_path);
  }

  LobsterReader::~LobsterReader() {
    if (msg_in_.is_open()) msg_in_.close();
    if (ob_in_.is_open())  ob_in_.close();
  }

  auto LobsterReader::next()
      -> std::optional<std::pair<Nanos, MEMarketUpdate>> {
    while (pending_.empty()) {
      if (!fillStep()) return std::nullopt;
    }
    auto p = pending_.front();
    pending_.pop_front();
    return p;
  }

  auto LobsterReader::fillStep() -> bool {
    if (eof_) return false;
    if (cfg_.max_events && count_ >= cfg_.max_events) { eof_ = true; return false; }

    std::string mline, oline;
    if (!std::getline(msg_in_, mline) || !std::getline(ob_in_, oline)) {
      eof_ = true;
      return false;
    }
    if (mline.empty() || oline.empty()) return true;   // skip blank, keep going
    ++count_;

    // ---- message: Time,Type,OrderID,Size,Price,Direction -------------------
    std::string ts_s, ty_s, oid_s, sz_s, px_s, dir_s;
    {
      std::stringstream ss(mline);
      std::getline(ss, ts_s, ',');
      std::getline(ss, ty_s, ',');
      std::getline(ss, oid_s, ',');
      std::getline(ss, sz_s, ',');
      std::getline(ss, px_s, ',');
      std::getline(ss, dir_s, ',');
    }
    const Nanos ts      = static_cast<Nanos>(std::llround(std::stod(ts_s) * 1e9));
    const int   ty      = std::stoi(ty_s);
    const Qty   msg_sz  = static_cast<Qty>(std::stoll(sz_s));
    const Price msg_px  = static_cast<Price>(std::llround(std::stod(px_s) / 100.0));
    const int   dir     = std::stoi(dir_s);

    auto makeUpd = [&](MarketUpdateType t, OrderId oid, Side side, Price px, Qty q,
                       Priority pr) {
      MEMarketUpdate u;
      u.type_      = t;
      u.order_id_  = oid;
      u.ticker_id_ = cfg_.ticker_id;
      u.side_      = side;
      u.price_     = px;
      u.qty_       = q;
      u.priority_  = pr;
      return u;
    };

    // ---- 1) execution -> TRADE print (drives fills + trade features) -------
    if (ty == 4 || ty == 5) {
      const Side aggressor = (dir == 1) ? Side::SELL : Side::BUY;  // buy LO hit by a seller, etc.
      pending_.push_back({ts, makeUpd(MarketUpdateType::TRADE, OrderId_INVALID, aggressor,
                                      msg_px, msg_sz, Priority_INVALID)});
    }

    // ---- 2) parse the orderbook snapshot (AskP,AskS,BidP,BidS) x levels ----
    std::unordered_map<Price, Qty> cur_bid, cur_ask;
    {
      std::stringstream ss(oline);
      std::string tok;
      std::vector<int64_t> v;
      v.reserve(4 * cfg_.levels);
      while (std::getline(ss, tok, ',') && static_cast<int>(v.size()) < 4 * cfg_.levels) {
        v.push_back(tok.empty() ? kSentinel : std::stoll(tok));
      }
      for (int i = 0; i + 3 < static_cast<int>(v.size()); i += 4) {
        const int64_t askP = v[i], askS = v[i + 1], bidP = v[i + 2], bidS = v[i + 3];
        if (std::llabs(askP) < kSentinel && askS > 0)
          cur_ask[static_cast<Price>(std::llround(askP / 100.0))] = static_cast<Qty>(askS);
        if (std::llabs(bidP) < kSentinel && bidS > 0)
          cur_bid[static_cast<Price>(std::llround(bidP / 100.0))] = static_cast<Qty>(bidS);
      }
    }

    // ---- 3) diff prev -> cur into ADD/MODIFY/CANCEL (cancels first) --------
    auto diffSide = [&](Side side, OrderId oid_base,
                        std::unordered_map<Price, Qty> &prev,
                        const std::unordered_map<Price, Qty> &cur) {
      for (const auto &[px, q] : prev) {
        (void)q;
        if (!cur.count(px))
          pending_.push_back({ts, makeUpd(MarketUpdateType::CANCEL, oid_base + px, side,
                                          px, 0, Priority_INVALID)});
      }
      for (const auto &[px, q] : cur) {
        const auto it = prev.find(px);
        if (it == prev.end())
          pending_.push_back({ts, makeUpd(MarketUpdateType::ADD, oid_base + px, side,
                                          px, q, next_priority_++)});
        else if (it->second != q)
          pending_.push_back({ts, makeUpd(MarketUpdateType::MODIFY, oid_base + px, side,
                                          px, q, Priority_INVALID)});
      }
    };
    diffSide(Side::BUY,  0,             prev_bid_, cur_bid);
    diffSide(Side::SELL, kAskOidOffset, prev_ask_, cur_ask);

    prev_bid_ = std::move(cur_bid);
    prev_ask_ = std::move(cur_ask);
    return true;
  }
}
