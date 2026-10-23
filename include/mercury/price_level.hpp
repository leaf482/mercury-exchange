#pragma once

#include "mercury/order.hpp"

#include <cassert>
#include <cstddef>
#include <deque>

namespace mercury {

// All orders at one price, oldest first (time priority).
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

  void dequeue() {
    assert(!orders_.empty());
    orders_.pop_front();
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

  Quantity total_quantity() const {
    std::uint64_t total = 0;
    for (const Order& order : orders_) {
      total += order.quantity.value();
    }
    return Quantity{total};
  }

 private:
  Price price_;
  std::deque<Order> orders_;
};

}  // namespace mercury
