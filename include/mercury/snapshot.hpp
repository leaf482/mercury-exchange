#pragma once

#include "mercury/jsonl.hpp"

#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mercury {
namespace snapshot {

using jsonl::detail::field;
using jsonl::detail::format_side;
using jsonl::detail::format_tif;
using jsonl::detail::optional_bool;
using jsonl::detail::optional_tif;
using jsonl::detail::parse_side;
using jsonl::detail::parse_tif;
using jsonl::detail::require_int;
using jsonl::detail::require_string;

inline const char* format_stp(SelfTradePrevention stp) {
  return stp == SelfTradePrevention::CancelResting ? "cancel_resting" : "off";
}

inline SelfTradePrevention parse_stp(std::string_view stp) {
  if (stp == "off") {
    return SelfTradePrevention::Off;
  }
  if (stp == "cancel_resting") {
    return SelfTradePrevention::CancelResting;
  }
  throw std::runtime_error("invalid stp");
}

inline std::string format_restart(const RestartSnapshot& snap) {
  std::ostringstream out;
  out << "{\"type\":\"meta\""
      << ",\"now\":" << snap.now << ",\"next_trade_id\":" << snap.next_trade_id
      << ",\"enforce_cash\":" << (snap.enforce_cash ? "true" : "false")
      << ",\"maker_bps\":" << snap.fees.maker_bps
      << ",\"taker_bps\":" << snap.fees.taker_bps
      << ",\"stp\":\"" << format_stp(snap.stp) << '"'
      << ",\"max_order_quantity\":" << snap.limits.max_order_quantity.value()
      << ",\"max_abs_position\":" << snap.limits.max_abs_position << "}\n";

  for (const RestartCash& row : snap.cash) {
    out << "{\"type\":\"cash\",\"account\":" << row.account.value()
        << ",\"amount\":" << row.amount << "}\n";
  }
  for (const RestartFees& row : snap.fees_paid) {
    out << "{\"type\":\"fees_paid\",\"account\":" << row.account.value()
        << ",\"amount\":" << row.amount << "}\n";
  }
  for (const RestartPosition& row : snap.positions) {
    out << "{\"type\":\"position\",\"account\":" << row.account.value()
        << ",\"symbol\":" << row.symbol.value() << ",\"quantity\":" << row.quantity
        << ",\"avg_ticks\":" << row.avg_ticks
        << ",\"realized_pnl\":" << row.realized_pnl << "}\n";
  }
  for (const RestartLastTrade& row : snap.last_trades) {
    out << "{\"type\":\"last_trade\",\"symbol\":" << row.symbol.value()
        << ",\"price\":" << row.price.ticks() << "}\n";
  }
  for (const RestartOrder& row : snap.orders) {
    const Order& order = row.order;
    out << "{\"type\":\"order\""
        << ",\"id\":" << order.id.value() << ",\"side\":\"" << format_side(order.side)
        << '"' << ",\"price\":" << order.price.ticks()
        << ",\"quantity\":" << order.quantity.value()
        << ",\"account\":" << order.account.value() << ",\"tif\":\""
        << format_tif(order.tif) << '"' << ",\"symbol\":" << order.symbol.value()
        << ",\"display\":" << order.display.value()
        << ",\"visible\":" << order.visible.value()
        << ",\"expire_at\":" << order.expire_at
        << ",\"cash_reserve_qty\":" << row.cash_reserve_qty.value() << "}\n";
  }
  for (const StopOrder& stop : snap.stops) {
    out << "{\"type\":\"stop\""
        << ",\"id\":" << stop.id.value() << ",\"side\":\"" << format_side(stop.side)
        << '"' << ",\"stop_price\":" << stop.stop_price.ticks()
        << ",\"quantity\":" << stop.quantity.value()
        << ",\"account\":" << stop.account.value();
    if (stop.limit_price) {
      out << ",\"limit_price\":" << stop.limit_price->ticks();
    }
    out << ",\"tif\":\"" << format_tif(stop.tif) << '"'
        << ",\"symbol\":" << stop.symbol.value() << ",\"expire_at\":" << stop.expire_at
        << "}\n";
  }
  return out.str();
}

inline RestartSnapshot parse_restart(std::string_view text) {
  RestartSnapshot snap;
  bool saw_meta = false;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    std::string_view line = text.substr(begin, end == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : end - begin);
    begin = end == std::string_view::npos ? text.size() : end + 1;
    if (line.empty() || line == "\r") {
      continue;
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const std::string type = require_string(line, "type");
    if (type == "meta") {
      saw_meta = true;
      snap.now = static_cast<std::uint64_t>(require_int(line, "now"));
      snap.next_trade_id =
          static_cast<std::uint64_t>(require_int(line, "next_trade_id"));
      snap.enforce_cash = optional_bool(line, "enforce_cash");
      snap.fees.maker_bps = require_int(line, "maker_bps");
      snap.fees.taker_bps = require_int(line, "taker_bps");
      snap.stp = parse_stp(require_string(line, "stp"));
      snap.limits.max_order_quantity =
          Quantity{static_cast<std::uint64_t>(require_int(line, "max_order_quantity"))};
      snap.limits.max_abs_position =
          static_cast<std::uint64_t>(require_int(line, "max_abs_position"));
      continue;
    }
    if (type == "cash") {
      snap.cash.push_back(RestartCash{
          .account = AccountId{static_cast<std::uint64_t>(require_int(line, "account"))},
          .amount = require_int(line, "amount"),
      });
      continue;
    }
    if (type == "fees_paid") {
      snap.fees_paid.push_back(RestartFees{
          .account = AccountId{static_cast<std::uint64_t>(require_int(line, "account"))},
          .amount = require_int(line, "amount"),
      });
      continue;
    }
    if (type == "position") {
      snap.positions.push_back(RestartPosition{
          .account = AccountId{static_cast<std::uint64_t>(require_int(line, "account"))},
          .symbol = Symbol{static_cast<std::uint64_t>(require_int(line, "symbol"))},
          .quantity = require_int(line, "quantity"),
          .avg_ticks = require_int(line, "avg_ticks"),
          .realized_pnl = require_int(line, "realized_pnl"),
      });
      continue;
    }
    if (type == "last_trade") {
      snap.last_trades.push_back(RestartLastTrade{
          .symbol = Symbol{static_cast<std::uint64_t>(require_int(line, "symbol"))},
          .price = Price{require_int(line, "price")},
      });
      continue;
    }
    if (type == "order") {
      Order order{
          .id = OrderId{static_cast<std::uint64_t>(require_int(line, "id"))},
          .side = parse_side(require_string(line, "side")),
          .price = Price{require_int(line, "price")},
          .quantity = Quantity{static_cast<std::uint64_t>(require_int(line, "quantity"))},
          .account =
              AccountId{static_cast<std::uint64_t>(require_int(line, "account"))},
          .tif = field(line, "tif") ? parse_tif(require_string(line, "tif"))
                                    : TimeInForce::Gtc,
          .symbol = Symbol{static_cast<std::uint64_t>(require_int(line, "symbol"))},
          .post_only = false,
          .reduce_only = false,
          .display =
              Quantity{static_cast<std::uint64_t>(require_int(line, "display"))},
          .visible =
              Quantity{static_cast<std::uint64_t>(require_int(line, "visible"))},
          .expire_at = static_cast<std::uint64_t>(require_int(line, "expire_at")),
      };
      snap.orders.push_back(RestartOrder{
          .order = order,
          .cash_reserve_qty = Quantity{static_cast<std::uint64_t>(
              require_int(line, "cash_reserve_qty"))},
      });
      continue;
    }
    if (type == "stop") {
      StopOrder stop{
          .id = OrderId{static_cast<std::uint64_t>(require_int(line, "id"))},
          .side = parse_side(require_string(line, "side")),
          .stop_price = Price{require_int(line, "stop_price")},
          .quantity =
              Quantity{static_cast<std::uint64_t>(require_int(line, "quantity"))},
          .account =
              AccountId{static_cast<std::uint64_t>(require_int(line, "account"))},
          .limit_price = std::nullopt,
          .tif = optional_tif(line),
          .symbol = Symbol{static_cast<std::uint64_t>(require_int(line, "symbol"))},
          .expire_at = static_cast<std::uint64_t>(require_int(line, "expire_at")),
      };
      if (field(line, "limit_price")) {
        stop.limit_price = Price{require_int(line, "limit_price")};
      }
      snap.stops.push_back(std::move(stop));
      continue;
    }
    throw std::runtime_error("unknown snapshot type: " + type);
  }
  if (!saw_meta) {
    throw std::runtime_error("snapshot missing meta");
  }
  return snap;
}

}  // namespace snapshot
}  // namespace mercury
