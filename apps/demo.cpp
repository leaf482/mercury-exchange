#include "mercury/order_book.hpp"

#include <iostream>

using mercury::AccountId;
using mercury::MarketOrder;
using mercury::Order;
using mercury::OrderBook;
using mercury::OrderId;
using mercury::Price;
using mercury::Quantity;
using mercury::Side;
using mercury::TimeInForce;

int main() {
  OrderBook book;

  book.add(Order{.id = OrderId{1},
                 .side = Side::Sell,
                 .price = Price{100},
                 .quantity = Quantity{5},
                 .account = AccountId{1}});

  // Limit buy: 3 fill at maker price 100.
  for (const auto& trade : book.add(Order{.id = OrderId{2},
                                          .side = Side::Buy,
                                          .price = Price{100},
                                          .quantity = Quantity{3},
                                          .account = AccountId{2}})) {
    std::cout << "limit trade price=" << trade.price.ticks()
              << " qty=" << trade.quantity.value() << '\n';
  }

  // IOC for 10 when only 2 remain: fills 2, discards 8 (no new bid).
  book.add(Order{.id = OrderId{3},
                 .side = Side::Buy,
                 .price = Price{100},
                 .quantity = Quantity{10},
                 .account = AccountId{2},
                 .tif = TimeInForce::Ioc});

  book.add(Order{.id = OrderId{4},
                 .side = Side::Sell,
                 .price = Price{105},
                 .quantity = Quantity{4},
                 .account = AccountId{1}});

  // Market buy sweeps whatever asks are left.
  for (const auto& trade : book.add_market(MarketOrder{.id = OrderId{5},
                                                       .side = Side::Buy,
                                                       .quantity = Quantity{2},
                                                       .account = AccountId{2}})) {
    std::cout << "market trade price=" << trade.price.ticks()
              << " qty=" << trade.quantity.value() << '\n';
  }

  if (const auto ask = book.best_ask()) {
    std::cout << "best ask=" << ask->ticks() << '\n';
  }
}
