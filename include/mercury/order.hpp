#pragma once

#include "mercury/types.hpp"

namespace mercury {

struct Order {
  OrderId id;
  Side side;
  Price price;
  Quantity quantity;
  AccountId account{0};

  constexpr bool operator==(const Order&) const = default;
};

}  // namespace mercury
