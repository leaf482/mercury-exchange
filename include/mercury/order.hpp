#pragma once

#include "mercury/types.hpp"

namespace mercury {

// Gtc: leftover quantity rests on the book.
// Ioc: match what you can, discard the rest (never rests).
enum class TimeInForce : std::uint8_t { Gtc, Ioc };

struct Order {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
  AccountId account{0};
  TimeInForce tif{TimeInForce::Gtc};

  constexpr bool operator==(const Order&) const = default;
};

// No limit price: take liquidity until filled or the book is empty.
struct MarketOrder {
  OrderId id;
  Side side;
  Quantity quantity;
  AccountId account{0};

  constexpr bool operator==(const MarketOrder&) const = default;
};

}  // namespace mercury
