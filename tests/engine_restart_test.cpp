#include "mercury/engine.hpp"
#include "mercury/snapshot.hpp"

#include <gtest/gtest.h>

using mercury::AccountId;
using mercury::Engine;
using mercury::FeeSchedule;
using mercury::Order;
using mercury::OrderId;
using mercury::Price;
using mercury::Quantity;
using mercury::RiskDecision;
using mercury::Side;
using mercury::StopOrder;
using mercury::Symbol;
using mercury::TimeInForce;
using mercury::Trade;
using mercury::snapshot::format_restart;
using mercury::snapshot::parse_restart;

namespace {

void expect_same_trades(const std::vector<Trade>& left, const std::vector<Trade>& right) {
  ASSERT_EQ(left.size(), right.size());
  for (std::size_t i = 0; i < left.size(); ++i) {
    EXPECT_EQ(left[i].id, right[i].id);
    EXPECT_EQ(left[i].maker_id, right[i].maker_id);
    EXPECT_EQ(left[i].taker_id, right[i].taker_id);
    EXPECT_EQ(left[i].price, right[i].price);
    EXPECT_EQ(left[i].quantity, right[i].quantity);
  }
}

Engine seed() {
  Engine engine{{}, {}, FeeSchedule{.maker_bps = 1, .taker_bps = 2}, true};
  engine.set_cash(AccountId{1}, 100'000);
  engine.set_cash(AccountId{2}, 100'000);

  EXPECT_EQ(engine
                .add(Order{.id = OrderId{1},
                           .side = Side::Buy,
                           .price = Price{100},
                           .quantity = Quantity{3},
                           .account = AccountId{1}})
                .decision,
            RiskDecision::Accept);
  EXPECT_EQ(engine
                .add(Order{.id = OrderId{2},
                           .side = Side::Buy,
                           .price = Price{100},
                           .quantity = Quantity{3},
                           .account = AccountId{1}})
                .decision,
            RiskDecision::Accept);
  EXPECT_EQ(engine
                .add(Order{.id = OrderId{3},
                           .side = Side::Sell,
                           .price = Price{110},
                           .quantity = Quantity{8},
                           .account = AccountId{2},
                           .symbol = Symbol{1},
                           .display = Quantity{2}})
                .decision,
            RiskDecision::Accept);
  EXPECT_EQ(engine
                .add(Order{.id = OrderId{4},
                           .side = Side::Buy,
                           .price = Price{110},
                           .quantity = Quantity{1},
                           .account = AccountId{1},
                           .symbol = Symbol{1}})
                .decision,
            RiskDecision::Accept);
  EXPECT_EQ(engine
                .add(Order{.id = OrderId{5},
                           .side = Side::Sell,
                           .price = Price{130},
                           .quantity = Quantity{2},
                           .account = AccountId{2},
                           .tif = TimeInForce::Gtd,
                           .expire_at = 10})
                .decision,
            RiskDecision::Accept);
  EXPECT_EQ(engine
                .add_stop(StopOrder{.id = OrderId{6},
                                    .side = Side::Buy,
                                    .stop_price = Price{140},
                                    .quantity = Quantity{1},
                                    .account = AccountId{1},
                                    .limit_price = std::nullopt,
                                    .tif = TimeInForce::Gtc,
                                    .symbol = Symbol{1},
                                    .expire_at = 0})
                .decision,
            RiskDecision::Accept);
  engine.advance_time(4);
  return engine;
}

}  // namespace

TEST(EngineRestart, ContinuesMatchingAfterLoad) {
  Engine original = seed();
  Engine restored;
  restored.load_restart(parse_restart(format_restart(original.restart_snapshot())));

  EXPECT_EQ(restored.now(), 4u);
  EXPECT_EQ(restored.next_trade_id(), original.next_trade_id());
  EXPECT_EQ(restored.cash(AccountId{1}), original.cash(AccountId{1}));
  EXPECT_EQ(restored.reserved_cash(AccountId{1}), original.reserved_cash(AccountId{1}));
  EXPECT_EQ(restored.fees_paid(AccountId{1}), original.fees_paid(AccountId{1}));
  EXPECT_EQ(restored.pending_stop_count(Symbol{1}), 1u);
  EXPECT_EQ(restored.snapshot(4, Symbol{1}).asks[0].quantity, Quantity{1});

  const Order take{.id = OrderId{7},
                   .side = Side::Sell,
                   .price = Price{100},
                   .quantity = Quantity{1},
                   .account = AccountId{2}};
  expect_same_trades(original.add(take).trades, restored.add(take).trades);
  EXPECT_EQ(original.snapshot(4).bids[0].quantity, restored.snapshot(4).bids[0].quantity);
  EXPECT_EQ(original.account_report(AccountId{1}).equity,
            restored.account_report(AccountId{1}).equity);
}

TEST(EngineRestart, ClockStillExpiresRestingGtd) {
  Engine original = seed();
  Engine restored;
  restored.load_restart(original.restart_snapshot());

  EXPECT_EQ(restored.advance_time(10), 1u);
  EXPECT_FALSE(restored.book().best_ask().has_value());
  EXPECT_EQ(restored.now(), 10u);
}

TEST(EngineRestart, PendingStopFiresAfterResume) {
  Engine original = seed();
  Engine restored;
  restored.load_restart(original.restart_snapshot());
  ASSERT_TRUE(original.cancel(OrderId{3}));
  ASSERT_TRUE(restored.cancel(OrderId{3}));

  const Order ask{.id = OrderId{8},
                  .side = Side::Sell,
                  .price = Price{140},
                  .quantity = Quantity{1},
                  .account = AccountId{2},
                  .symbol = Symbol{1}};
  const Order bid{.id = OrderId{9},
                  .side = Side::Buy,
                  .price = Price{140},
                  .quantity = Quantity{1},
                  .account = AccountId{1},
                  .symbol = Symbol{1}};
  ASSERT_TRUE(original.add(ask).trades.empty());
  ASSERT_TRUE(restored.add(ask).trades.empty());
  expect_same_trades(original.add(bid).trades, restored.add(bid).trades);
  EXPECT_EQ(original.pending_stop_count(Symbol{1}), 0u);
  EXPECT_EQ(restored.pending_stop_count(Symbol{1}), 0u);
  EXPECT_EQ(original.last_trade_price(Symbol{1}), Price{140});
  EXPECT_EQ(restored.last_trade_price(Symbol{1}), Price{140});
}
