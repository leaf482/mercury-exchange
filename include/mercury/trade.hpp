#pragma once

#include "mercury/types.hpp"

namespace mercury {

// One fill. Trade price is always the resting (maker) price.
struct Trade {
  OrderId maker_id;
  OrderId taker_id;
  AccountId maker_account{0};
  AccountId taker_account{0};
  Price price;
  Quantity quantity;

  constexpr bool operator==(const Trade&) const = default;
};

}  // namespace mercury
