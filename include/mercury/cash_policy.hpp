#pragma once

#include "mercury/balances.hpp"
#include "mercury/order_book.hpp"
#include "mercury/positions.hpp"
#include "mercury/risk.hpp"

#include <algorithm>
#include <cstdint>

namespace mercury {

// Free long inventory after subtracting resting sells (cover for shorts).
inline Quantity free_long(std::int64_t position, std::uint64_t working_sell) {
  const std::int64_t long_qty = position > 0 ? position : 0;
  const std::int64_t free = long_qty - static_cast<std::int64_t>(working_sell);
  return Quantity{static_cast<std::uint64_t>(free > 0 ? free : 0)};
}

inline Quantity short_qty(std::int64_t position, Quantity quantity,
                          std::uint64_t working_sell) {
  const std::uint64_t cover = free_long(position, working_sell).value();
  if (quantity.value() <= cover) {
    return Quantity{0};
  }
  return Quantity{quantity.value() - cover};
}

// Worst-case buy cost at limit price, or walking asks for market.
inline RiskDecision check_buy_cash(bool enforce_cash, const Balances& balances,
                                   const OrderBook& book, AccountId account,
                                   Price limit, Quantity quantity, bool is_market) {
  if (!enforce_cash) {
    return RiskDecision::Accept;
  }

  std::int64_t need = 0;
  if (!is_market) {
    need = limit.ticks() * static_cast<std::int64_t>(quantity.value());
  } else {
    need = book.estimate_buy_notional(quantity, true);
  }

  if (balances.available(account) < need) {
    return RiskDecision::InsufficientCash;
  }
  return RiskDecision::Accept;
}

// Margin for the uncovered (short) portion of a sell.
inline RiskDecision check_sell_margin(bool enforce_cash, const Balances& balances,
                                      const OrderBook& book, std::int64_t position,
                                      AccountId account, Price limit, Quantity quantity,
                                      bool is_market, std::uint64_t working_sell) {
  if (!enforce_cash) {
    return RiskDecision::Accept;
  }
  const Quantity uncovered = short_qty(position, quantity, working_sell);
  if (uncovered.is_zero()) {
    return RiskDecision::Accept;
  }

  std::int64_t need = 0;
  if (!is_market) {
    need = limit.ticks() * static_cast<std::int64_t>(uncovered.value());
  } else {
    need = book.estimate_sell_notional(uncovered, true);
  }

  if (balances.available(account) < need) {
    return RiskDecision::InsufficientCash;
  }
  return RiskDecision::Accept;
}

// Cash to reserve for a GTC/GTD rest after a (possibly partial) fill.
// Buys reserve the full rest; sells reserve only the uncovered short rest.
// `working_sell` and `position` are the pre-trade values.
inline Quantity resting_cash_reserve(bool enforce_cash, Side side, Price price,
                                     Quantity rested, Quantity filled,
                                     std::int64_t position,
                                     std::uint64_t working_sell) {
  if (!enforce_cash || price.ticks() == 0 || rested.is_zero()) {
    return Quantity{0};
  }
  if (side == Side::Buy) {
    return rested;
  }
  const std::uint64_t cover = free_long(position, working_sell).value();
  const std::uint64_t covered_fill = std::min(filled.value(), cover);
  const std::uint64_t cover_left = cover - covered_fill;
  const std::uint64_t short_rest =
      rested.value() > cover_left ? rested.value() - cover_left : 0;
  return Quantity{short_rest};
}

}  // namespace mercury
