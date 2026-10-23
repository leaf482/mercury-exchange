#include "mercury/order_book.hpp"

#include <gtest/gtest.h>

using mercury::AccountId;
using mercury::Order;
using mercury::OrderBook;
using mercury::OrderId;
using mercury::Price;
using mercury::Quantity;
using mercury::Side;

TEST(OrderBook, RestsWithoutMatch) {
  OrderBook book;
  const auto trades = book.add(Order{.id = OrderId{1},
                                     .side = Side::Buy,
                                     .price = Price{100},
                                     .quantity = Quantity{2},
                                     .account = AccountId{1}});
  EXPECT_TRUE(trades.empty());
  EXPECT_EQ(book.best_bid(), Price{100});
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, MatchesAtMakerPrice) {
  OrderBook book;
  book.add(Order{.id = OrderId{1},
                 .side = Side::Sell,
                 .price = Price{100},
                 .quantity = Quantity{5},
                 .account = AccountId{1}});

  const auto trades = book.add(Order{.id = OrderId{2},
                                     .side = Side::Buy,
                                     .price = Price{101},
                                     .quantity = Quantity{3},
                                     .account = AccountId{2}});

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, OrderId{1});
  EXPECT_EQ(trades[0].taker_id, OrderId{2});
  EXPECT_EQ(trades[0].price, Price{100});  // maker price, not 101
  EXPECT_EQ(trades[0].quantity, Quantity{3});
  EXPECT_EQ(book.best_ask(), Price{100});
}

TEST(OrderBook, TimePriorityAtSamePrice) {
  OrderBook book;
  book.add(Order{.id = OrderId{1},
                 .side = Side::Sell,
                 .price = Price{100},
                 .quantity = Quantity{1},
                 .account = AccountId{1}});
  book.add(Order{.id = OrderId{2},
                 .side = Side::Sell,
                 .price = Price{100},
                 .quantity = Quantity{1},
                 .account = AccountId{1}});

  const auto trades = book.add(Order{.id = OrderId{3},
                                     .side = Side::Buy,
                                     .price = Price{100},
                                     .quantity = Quantity{1},
                                     .account = AccountId{2}});

  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].maker_id, OrderId{1});  // older sell first
}

TEST(OrderBook, CancelRemovesRestingOrder) {
  OrderBook book;
  book.add(Order{.id = OrderId{1},
                 .side = Side::Buy,
                 .price = Price{100},
                 .quantity = Quantity{2},
                 .account = AccountId{1}});
  EXPECT_TRUE(book.cancel(OrderId{1}));
  EXPECT_FALSE(book.best_bid().has_value());
  EXPECT_FALSE(book.cancel(OrderId{1}));
}
