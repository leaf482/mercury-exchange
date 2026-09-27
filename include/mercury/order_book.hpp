#pragma once

#include "mercury/price_level.hpp"
#include "mercury/trade.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mercury {

struct BookLevel {
  Price price;
  Quantity quantity;
  std::size_t order_count = 0;

  constexpr bool operator==(const BookLevel&) const = default;
};

struct BookSnapshot {
  std::vector<BookLevel> bids;  // best bid first
  std::vector<BookLevel> asks;  // best ask first

  bool operator==(const BookSnapshot&) const = default;

  std::optional<Price> best_bid() const {
    if (bids.empty()) {
      return std::nullopt;
    }
    return bids.front().price;
  }

  std::optional<Price> best_ask() const {
    if (asks.empty()) {
      return std::nullopt;
    }
    return asks.front().price;
  }

  std::optional<std::int64_t> spread_ticks() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
      return std::nullopt;
    }
    return ask->ticks() - bid->ticks();
  }
};

class OrderBook {
 public:
  explicit OrderBook(SelfTradePrevention stp = SelfTradePrevention::Off) : stp_(stp) {}

  void set_self_trade_prevention(SelfTradePrevention stp) { stp_ = stp; }

  SelfTradePrevention self_trade_prevention() const { return stp_; }

  // True if a limit at this price would immediately match.
  bool would_take(const Order& order) const {
    if (order.side == Side::Buy) {
      const auto ask = best_ask();
      return ask && order.price >= *ask;
    }
    const auto bid = best_bid();
    return bid && order.price <= *bid;
  }

  // Match against the opposite side. GTC rests remainder; IOC/FOK do not.
  // post_only that would take returns no trades and does not rest.
  std::vector<Trade> add(Order order) {
    stp_cancels_.clear();
    if (order.post_only && would_take(order)) {
      return {};
    }
    if (order.tif == TimeInForce::Fok && !can_fully_fill(order)) {
      return {};
    }

    std::vector<Trade> trades;

    if (order.side == Side::Buy) {
      match_buy(order, trades, false);
    } else {
      match_sell(order, trades, false);
    }

    if (rests_on_book(order.tif) && !order.quantity.is_zero()) {
      rest(std::move(order));
    }

    return trades;
  }

  // Fill against available liquidity; any unfilled quantity is discarded.
  std::vector<Trade> add_market(MarketOrder order) {
    stp_cancels_.clear();
    Order taker{
        .id = order.id,
        .side = order.side,
        .price = Price{0},
        .quantity = order.quantity,
        .account = order.account,
        .tif = TimeInForce::Gtc,
        .symbol = order.symbol,
    };

    std::vector<Trade> trades;
    if (taker.side == Side::Buy) {
      match_buy(taker, trades, true);
    } else {
      match_sell(taker, trades, true);
    }
    return trades;
  }

  // OrderIds cancelled by self-trade prevention during the last add/add_market.
  std::vector<OrderId> take_stp_cancels() { return std::move(stp_cancels_); }

  // Cancel-replace a resting order (loses time priority). qty 0 => cancel only.
  // Returns nullopt if id is not resting.
  std::optional<std::vector<Trade>> replace(OrderId id, Price price, Quantity quantity) {
    stp_cancels_.clear();
    const auto loc_it = index_.find(id);
    if (loc_it == index_.end()) {
      return std::nullopt;
    }

    const RestingLocation loc = loc_it->second;
    std::optional<Order> original;
    if (loc.side == Side::Buy) {
      original = bids_.at(loc.price).find(id);
    } else {
      original = asks_.at(loc.price).find(id);
    }
    assert(original.has_value());

    cancel(id);
    if (quantity.is_zero()) {
      return std::vector<Trade>{};
    }

    return add(Order{
        .id = id,
        .side = original->side,
        .price = price,
        .quantity = quantity,
        .account = original->account,
        .tif = original->tif == TimeInForce::Gtd ? TimeInForce::Gtd : TimeInForce::Gtc,
        .symbol = original->symbol,
        .post_only = original->post_only,
        .reduce_only = original->reduce_only,
        .display = original->display,
        .expire_at = original->expire_at,
    });
  }


  // Removes a resting order. Returns false if the id is not on the book.
  bool cancel(OrderId id) {
    const auto loc_it = index_.find(id);
    if (loc_it == index_.end()) {
      return false;
    }

    const RestingLocation loc = loc_it->second;
    if (loc.side == Side::Buy) {
      erase_from(bids_, loc.price, id);
    } else {
      erase_from(asks_, loc.price, id);
    }

    index_.erase(loc_it);
    return true;
  }

  std::optional<Price> best_bid() const {
    if (bids_.empty()) {
      return std::nullopt;
    }
    return bids_.begin()->first;
  }

  std::optional<Price> best_ask() const {
    if (asks_.empty()) {
      return std::nullopt;
    }
    return asks_.begin()->first;
  }

  // Place an already-resting order without matching or re-arming the iceberg tip.
  void restore(Order order) {
    const OrderId id = order.id;
    const RestingLocation loc{order.side, order.price};
    if (order.side == Side::Buy) {
      auto [it, _] = bids_.try_emplace(order.price, order.price);
      it->second.enqueue(std::move(order));
    } else {
      auto [it, _] = asks_.try_emplace(order.price, order.price);
      it->second.enqueue(std::move(order));
    }
    index_.emplace(id, loc);
  }

  // Price-time order: best bid first, then best ask, FIFO within a price.
  std::vector<Order> resting_orders() const {
    std::vector<Order> out;
    auto append = [&](const auto& levels) {
      for (const auto& [price, level] : levels) {
        (void)price;
        for (const Order& order : level.orders()) {
          out.push_back(order);
        }
      }
    };
    append(bids_);
    append(asks_);
    return out;
  }

  BookSnapshot snapshot(std::size_t max_levels) const {
    BookSnapshot snap;
    for (const auto& [price, level] : bids_) {
      if (snap.bids.size() >= max_levels) {
        break;
      }
      snap.bids.push_back(BookLevel{
          .price = price,
          .quantity = level.visible_quantity(),
          .order_count = level.size(),
      });
    }
    for (const auto& [price, level] : asks_) {
      if (snap.asks.size() >= max_levels) {
        break;
      }
      snap.asks.push_back(BookLevel{
          .price = price,
          .quantity = level.visible_quantity(),
          .order_count = level.size(),
      });
    }
    return snap;
  }

  // Full (incl. iceberg hidden) ask walk for cash checks.
  std::int64_t estimate_buy_notional(Quantity quantity, bool is_market,
                                     Price limit = Price{0}) const {
    std::int64_t need = 0;
    std::uint64_t remaining = quantity.value();
    for (const auto& [price, level] : asks_) {
      if (remaining == 0) {
        break;
      }
      if (!is_market && price > limit) {
        break;
      }
      const std::uint64_t take = std::min(remaining, level.total_quantity().value());
      need += price.ticks() * static_cast<std::int64_t>(take);
      remaining -= take;
    }
    return need;
  }

  // Full bid walk for short-margin cash checks on market sells.
  std::int64_t estimate_sell_notional(Quantity quantity, bool is_market,
                                      Price limit = Price{0}) const {
    std::int64_t need = 0;
    std::uint64_t remaining = quantity.value();
    for (const auto& [price, level] : bids_) {
      if (remaining == 0) {
        break;
      }
      if (!is_market && price < limit) {
        break;
      }
      const std::uint64_t take = std::min(remaining, level.total_quantity().value());
      need += price.ticks() * static_cast<std::int64_t>(take);
      remaining -= take;
    }
    return need;
  }

 private:
  struct RestingLocation {
    Side side;
    Price price;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<>>;
  using AskLevels = std::map<Price, PriceLevel>;

  void rest(Order order) {
    arm_iceberg(order);
    const OrderId id = order.id;
    const RestingLocation loc{order.side, order.price};

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
        break;
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
        break;
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
      if (is_self_trade(maker, taker)) {
        stp_cancels_.push_back(maker.id);
        index_.erase(maker.id);
        level.dequeue();
        continue;
      }

      if (!maker.display.is_zero() && maker.visible.is_zero()) {
        arm_iceberg(maker);
      }

      const Quantity tip = maker.display.is_zero() ? maker.quantity : maker.visible;
      const Quantity fill = std::min(taker.quantity, tip);

      trades.push_back(Trade{
          .maker_id = maker.id,
          .taker_id = taker.id,
          .maker_account = maker.account,
          .taker_account = taker.account,
          .price = maker.price,
          .quantity = fill,
          .symbol = taker.symbol,
      });

      taker.quantity = taker.quantity - fill;
      maker.quantity = maker.quantity - fill;
      if (!maker.display.is_zero()) {
        maker.visible = maker.visible - fill;
      }

      if (maker.quantity.is_zero()) {
        index_.erase(maker.id);
        level.dequeue();
      } else if (!maker.display.is_zero() && maker.visible.is_zero()) {
        arm_iceberg(maker);
        level.requeue_front();
      }
    }
  }

  bool is_self_trade(const Order& maker, const Order& taker) const {
    return stp_ == SelfTradePrevention::CancelResting &&
           maker.account == taker.account && maker.account.value() != 0;
  }

  bool can_fully_fill(const Order& order) const {
    std::uint64_t available = 0;
    if (order.side == Side::Buy) {
      for (const auto& [price, level] : asks_) {
        if (price > order.price) {
          break;
        }
        available += level.matchable_quantity(order.account, stp_).value();
        if (available >= order.quantity.value()) {
          return true;
        }
      }
    } else {
      for (const auto& [price, level] : bids_) {
        if (price < order.price) {
          break;
        }
        available += level.matchable_quantity(order.account, stp_).value();
        if (available >= order.quantity.value()) {
          return true;
        }
      }
    }
    return false;
  }

  template <typename Levels>
  static void erase_from(Levels& levels, Price price, OrderId id) {
    const auto level_it = levels.find(price);
    assert(level_it != levels.end());
    [[maybe_unused]] const bool erased = level_it->second.erase(id);
    assert(erased);
    if (level_it->second.empty()) {
      levels.erase(level_it);
    }
  }

  // Best bid = highest price, best ask = lowest price.
  BidLevels bids_;
  AskLevels asks_;
  std::map<OrderId, RestingLocation> index_;
  SelfTradePrevention stp_{SelfTradePrevention::Off};
  std::vector<OrderId> stp_cancels_{};
};

}  // namespace mercury
