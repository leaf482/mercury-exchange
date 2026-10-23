#pragma once

#include "mercury/price_level.hpp"
#include "mercury/trade.hpp"

#include <cassert>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mercury {

// Single-instrument limit book with price-time priority.
//
// Bids: higher price is better (std::greater).
// Asks: lower price is better (std::less, the default).
// Within a price: FIFO via PriceLevel's deque.
class OrderBook {
 public:
  // Match against the opposite side. GTC rests leftover; IOC discards it.
  std::vector<Trade> add(Order order) {
    std::vector<Trade> trades;
    if (order.side == Side::Buy) {
      match_buy(order, trades, /*is_market=*/false);
    } else {
      match_sell(order, trades, /*is_market=*/false);
    }
    if (order.tif == TimeInForce::Gtc && !order.quantity.is_zero()) {
      rest(std::move(order));
    }
    return trades;
  }

  // Sweep the opposite side; unfilled quantity is discarded.
  std::vector<Trade> add_market(MarketOrder order) {
    Order taker{
        .id = order.id,
        .side = order.side,
        .price = Price{0},
        .quantity = order.quantity,
        .account = order.account,
        .tif = TimeInForce::Ioc,
    };
    std::vector<Trade> trades;
    if (taker.side == Side::Buy) {
      match_buy(taker, trades, /*is_market=*/true);
    } else {
      match_sell(taker, trades, /*is_market=*/true);
    }
    return trades;
  }

  bool cancel(OrderId id) {
    const auto loc = index_.find(id);
    if (loc == index_.end()) {
      return false;
    }
    if (loc->second.side == Side::Buy) {
      erase_from(bids_, loc->second.price, id);
    } else {
      erase_from(asks_, loc->second.price, id);
    }
    index_.erase(loc);
    return true;
  }

  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional{bids_.begin()->first};
  }

  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional{asks_.begin()->first};
  }

 private:
  struct Location {
    Side side;
    Price price;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<>>;
  using AskLevels = std::map<Price, PriceLevel>;

  void rest(Order order) {
    const OrderId id = order.id;
    const Location loc{order.side, order.price};
    if (order.side == Side::Buy) {
      auto [it, _] = bids_.try_emplace(order.price, order.price);
      it->second.enqueue(std::move(order));
    } else {
      auto [it, _] = asks_.try_emplace(order.price, order.price);
      it->second.enqueue(std::move(order));
    }
    index_.emplace(id, loc);
  }

  void match_buy(Order& taker, std::vector<Trade>& trades, bool is_market) {
    while (!taker.quantity.is_zero() && !asks_.empty()) {
      auto level_it = asks_.begin();
      if (!is_market && level_it->first > taker.price) {
        break;  // next ask is too expensive for this limit
      }
      fill_level(level_it->second, taker, trades);
      if (level_it->second.empty()) {
        asks_.erase(level_it);
      }
    }
  }

  void match_sell(Order& taker, std::vector<Trade>& trades, bool is_market) {
    while (!taker.quantity.is_zero() && !bids_.empty()) {
      auto level_it = bids_.begin();
      if (!is_market && level_it->first < taker.price) {
        break;  // next bid is too cheap for this limit
      }
      fill_level(level_it->second, taker, trades);
      if (level_it->second.empty()) {
        bids_.erase(level_it);
      }
    }
  }

  void fill_level(PriceLevel& level, Order& taker, std::vector<Trade>& trades) {
    while (!taker.quantity.is_zero() && !level.empty()) {
      Order& maker = level.front();
      const Quantity fill = taker.quantity < maker.quantity ? taker.quantity
                                                            : maker.quantity;
      trades.push_back(Trade{
          .maker_id = maker.id,
          .taker_id = taker.id,
          .maker_account = maker.account,
          .taker_account = taker.account,
          .price = maker.price,  // maker price
          .quantity = fill,
      });
      taker.quantity = taker.quantity - fill;
      maker.quantity = maker.quantity - fill;
      if (maker.quantity.is_zero()) {
        index_.erase(maker.id);
        level.dequeue();
      }
    }
  }

  template <typename Levels>
  static void erase_from(Levels& levels, Price price, OrderId id) {
    const auto it = levels.find(price);
    assert(it != levels.end());
    it->second.erase(id);
    if (it->second.empty()) {
      levels.erase(it);
    }
  }

  BidLevels bids_;
  AskLevels asks_;
  std::map<OrderId, Location> index_;
};

}  // namespace mercury
