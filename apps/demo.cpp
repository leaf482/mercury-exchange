#include "mercury/order_book.hpp"

#include <iostream>

using mercury::AccountId;
using mercury::Order;
using mercury::OrderBook;
using mercury::OrderId;
using mercury::Price;
using mercury::Quantity;
using mercury::Side;

int main() {
  OrderBook book;

  // Rest a sell at 100.
  book.add(Order{.id = OrderId{1},
                 .side = Side::Sell,
                 .price = Price{100},
                 .quantity = Quantity{5},
                 .account = AccountId{1}});

  // Buy crosses it: 3 lots fill at the maker price (100), 2 lots rest.
  const auto trades = book.add(Order{.id = OrderId{2},
                                     .side = Side::Buy,
                                     .price = Price{100},
                                     .quantity = Quantity{3},
                                     .account = AccountId{2}});

  for (const auto& trade : trades) {
    std::cout << "trade maker=" << trade.maker_id.value()
              << " taker=" << trade.taker_id.value()
              << " price=" << trade.price.ticks()
              << " qty=" << trade.quantity.value() << '\n';
  }

  if (const auto ask = book.best_ask()) {
    std::cout << "best ask=" << ask->ticks() << '\n';
  }
  if (const auto bid = book.best_bid()) {
    std::cout << "best bid=" << bid->ticks() << '\n';
  }
}
