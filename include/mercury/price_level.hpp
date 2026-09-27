#pragma once

#include "mercury/order.hpp"

#include <cassert>
#include <cstddef>
#include <deque>
#include <optional>

namespace mercury {

// Orders at one price, oldest first (time priority).
class PriceLevel {
 public:
  explicit PriceLevel(Price price) : price_(price) {}

  Price price() const { return price_; }

  bool empty() const { return orders_.empty(); }

  std::size_t size() const { return orders_.size(); }

  void enqueue(Order order) {
    assert(order.price == price_);
    orders_.push_back(std::move(order));
  }

  Order& front() {
    assert(!orders_.empty());
    return orders_.front();
  }

  const Order& front() const {
    assert(!orders_.empty());
    return orders_.front();
  }

  void dequeue() {
    assert(!orders_.empty());
    orders_.pop_front();
  }

  // Move front order to the back (iceberg tip refill loses time priority).
  void requeue_front() {
    assert(!orders_.empty());
    Order order = std::move(orders_.front());
    orders_.pop_front();
    orders_.push_back(std::move(order));
  }

  bool erase(OrderId id) {
    for (auto it = orders_.begin(); it != orders_.end(); ++it) {
      if (it->id == id) {
        orders_.erase(it);
        return true;
      }
    }
    return false;
  }

  std::optional<Order> find(OrderId id) const {
    for (const Order& order : orders_) {
      if (order.id == id) {
        return order;
      }
    }
    return std::nullopt;
  }

  Quantity total_quantity() const {
    std::uint64_t total = 0;
    for (const Order& order : orders_) {
      total += order.quantity.value();
    }
    return Quantity{total};
  }

  // Book depth: iceberg peaks only (hidden size excluded).
  const std::deque<Order>& orders() const { return orders_; }

  Quantity visible_quantity() const {
    std::uint64_t total = 0;
    for (const Order& order : orders_) {
      total += mercury::visible_quantity(order).value();
    }
    return Quantity{total};
  }

  // Quantity that can fill `taker` under the given STP policy.
  Quantity matchable_quantity(AccountId taker_account,
                              SelfTradePrevention stp) const {
    std::uint64_t total = 0;
    for (const Order& order : orders_) {
      if (stp == SelfTradePrevention::CancelResting &&
          order.account == taker_account && order.account.value() != 0) {
        continue;
      }
      total += order.quantity.value();
    }
    return Quantity{total};
  }

 private:
  Price price_;
  std::deque<Order> orders_;
};

}  // namespace mercury
