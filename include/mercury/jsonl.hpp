#pragma once

#include "mercury/event_log.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace mercury {
namespace jsonl {
namespace detail {

inline std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return text;
}

inline std::optional<std::string_view> field(std::string_view line, std::string_view key) {
  const std::string pattern = "\"" + std::string(key) + "\":";
  const auto pos = line.find(pattern);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }

  std::string_view value = line.substr(pos + pattern.size());
  value = trim(value);
  if (value.empty()) {
    return std::nullopt;
  }

  if (value.front() == '"') {
    const auto end = value.find('"', 1);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }
    return value.substr(1, end - 1);
  }

  std::size_t end = 0;
  while (end < value.size() &&
         (std::isdigit(static_cast<unsigned char>(value[end])) || value[end] == '-')) {
    ++end;
  }
  return value.substr(0, end);
}

inline std::int64_t require_int(std::string_view line, std::string_view key) {
  const auto value = field(line, key);
  if (!value) {
    throw std::runtime_error("missing int field: " + std::string(key));
  }
  return std::stoll(std::string(*value));
}

inline std::string require_string(std::string_view line, std::string_view key) {
  const auto value = field(line, key);
  if (!value) {
    throw std::runtime_error("missing string field: " + std::string(key));
  }
  return std::string(*value);
}

inline Side parse_side(std::string_view side) {
  if (side == "buy") {
    return Side::Buy;
  }
  if (side == "sell") {
    return Side::Sell;
  }
  throw std::runtime_error("invalid side");
}

inline const char* format_side(Side side) {
  return side == Side::Buy ? "buy" : "sell";
}

inline TimeInForce parse_tif(std::string_view tif) {
  if (tif == "gtc") {
    return TimeInForce::Gtc;
  }
  if (tif == "ioc") {
    return TimeInForce::Ioc;
  }
  if (tif == "fok") {
    return TimeInForce::Fok;
  }
  if (tif == "gtd") {
    return TimeInForce::Gtd;
  }
  throw std::runtime_error("invalid tif");
}

inline const char* format_tif(TimeInForce tif) {
  switch (tif) {
    case TimeInForce::Gtc:
      return "gtc";
    case TimeInForce::Ioc:
      return "ioc";
    case TimeInForce::Fok:
      return "fok";
    case TimeInForce::Gtd:
      return "gtd";
  }
  return "gtc";
}

inline TimeInForce optional_tif(std::string_view line) {
  const auto value = field(line, "tif");
  return value ? parse_tif(*value) : TimeInForce::Gtc;
}

inline bool optional_bool(std::string_view line, std::string_view key, bool fallback = false) {
  const std::string pattern = "\"" + std::string(key) + "\":";
  const auto pos = line.find(pattern);
  if (pos == std::string_view::npos) {
    return fallback;
  }
  std::string_view value = trim(line.substr(pos + pattern.size()));
  if (value.size() >= 4 && value.substr(0, 4) == "true") {
    return true;
  }
  if (value.size() >= 5 && value.substr(0, 5) == "false") {
    return false;
  }
  throw std::runtime_error("invalid bool field: " + std::string(key));
}

inline RiskDecision parse_decision(std::string_view decision) {
  if (decision == "accept") {
    return RiskDecision::Accept;
  }
  if (decision == "order_too_large") {
    return RiskDecision::OrderTooLarge;
  }
  if (decision == "position_limit") {
    return RiskDecision::PositionLimit;
  }
  if (decision == "post_only") {
    return RiskDecision::PostOnly;
  }
  if (decision == "reduce_only") {
    return RiskDecision::ReduceOnly;
  }
  if (decision == "insufficient_cash") {
    return RiskDecision::InsufficientCash;
  }
  if (decision == "invalid_expire") {
    return RiskDecision::InvalidExpire;
  }
  throw std::runtime_error("invalid decision");
}

inline const char* format_decision(RiskDecision decision) {
  switch (decision) {
    case RiskDecision::Accept:
      return "accept";
    case RiskDecision::OrderTooLarge:
      return "order_too_large";
    case RiskDecision::PositionLimit:
      return "position_limit";
    case RiskDecision::PostOnly:
      return "post_only";
    case RiskDecision::ReduceOnly:
      return "reduce_only";
    case RiskDecision::InsufficientCash:
      return "insufficient_cash";
    case RiskDecision::InvalidExpire:
      return "invalid_expire";
  }
  return "accept";
}

inline Order parse_limit_order(std::string_view line) {
  return Order{
      .id = OrderId{static_cast<std::uint64_t>(require_int(line, "id"))},
      .side = parse_side(require_string(line, "side")),
      .price = Price{require_int(line, "price")},
      .quantity = Quantity{static_cast<std::uint64_t>(require_int(line, "quantity"))},
      .account = AccountId{static_cast<std::uint64_t>(
          field(line, "account") ? require_int(line, "account") : 0)},
      .tif = optional_tif(line),
      .symbol = Symbol{static_cast<std::uint64_t>(
          field(line, "symbol") ? require_int(line, "symbol") : 0)},
      .post_only = optional_bool(line, "post_only"),
      .reduce_only = optional_bool(line, "reduce_only"),
      .display = Quantity{static_cast<std::uint64_t>(
          field(line, "display") ? require_int(line, "display") : 0)},
      .expire_at = static_cast<std::uint64_t>(
          field(line, "expire_at") ? require_int(line, "expire_at") : 0),
  };
}

inline MarketOrder parse_market_order(std::string_view line) {
  return MarketOrder{
      .id = OrderId{static_cast<std::uint64_t>(require_int(line, "id"))},
      .side = parse_side(require_string(line, "side")),
      .quantity = Quantity{static_cast<std::uint64_t>(require_int(line, "quantity"))},
      .account = AccountId{static_cast<std::uint64_t>(
          field(line, "account") ? require_int(line, "account") : 0)},
      .symbol = Symbol{static_cast<std::uint64_t>(
          field(line, "symbol") ? require_int(line, "symbol") : 0)},
      .reduce_only = optional_bool(line, "reduce_only"),
  };
}

inline StopOrder parse_stop_order(std::string_view line) {
  StopOrder stop{
      .id = OrderId{static_cast<std::uint64_t>(require_int(line, "id"))},
      .side = parse_side(require_string(line, "side")),
      .stop_price = Price{require_int(line, "stop_price")},
      .quantity = Quantity{static_cast<std::uint64_t>(require_int(line, "quantity"))},
      .account = AccountId{static_cast<std::uint64_t>(
          field(line, "account") ? require_int(line, "account") : 0)},
      .limit_price = std::nullopt,
      .tif = optional_tif(line),
      .symbol = Symbol{static_cast<std::uint64_t>(
          field(line, "symbol") ? require_int(line, "symbol") : 0)},
      .expire_at = static_cast<std::uint64_t>(
          field(line, "expire_at") ? require_int(line, "expire_at") : 0),
  };
  if (field(line, "limit_price")) {
    stop.limit_price = Price{require_int(line, "limit_price")};
  }
  return stop;
}

// Shared body fields for event / reject lines (no surrounding type / braces).
inline void append_limit_fields(std::ostream& out, const Order& order) {
  out << ",\"id\":" << order.id.value() << ",\"side\":\"" << format_side(order.side)
      << '"' << ",\"price\":" << order.price.ticks()
      << ",\"quantity\":" << order.quantity.value()
      << ",\"account\":" << order.account.value() << ",\"tif\":\""
      << format_tif(order.tif) << '"' << ",\"symbol\":" << order.symbol.value();
  if (order.post_only) {
    out << ",\"post_only\":true";
  }
  if (order.reduce_only) {
    out << ",\"reduce_only\":true";
  }
  if (!order.display.is_zero()) {
    out << ",\"display\":" << order.display.value();
  }
  if (order.expire_at != 0) {
    out << ",\"expire_at\":" << order.expire_at;
  }
}

inline void append_market_fields(std::ostream& out, const MarketOrder& order) {
  out << ",\"id\":" << order.id.value() << ",\"side\":\"" << format_side(order.side)
      << '"' << ",\"quantity\":" << order.quantity.value()
      << ",\"account\":" << order.account.value()
      << ",\"symbol\":" << order.symbol.value();
  if (order.reduce_only) {
    out << ",\"reduce_only\":true";
  }
}

// Event lines omit expire_at when 0; restart snapshots always write it.
inline void append_stop_fields(std::ostream& out, const StopOrder& stop,
                               bool always_expire = false) {
  out << ",\"id\":" << stop.id.value() << ",\"side\":\"" << format_side(stop.side) << '"'
      << ",\"stop_price\":" << stop.stop_price.ticks()
      << ",\"quantity\":" << stop.quantity.value()
      << ",\"account\":" << stop.account.value();
  if (stop.limit_price) {
    out << ",\"limit_price\":" << stop.limit_price->ticks();
  }
  out << ",\"tif\":\"" << format_tif(stop.tif) << '"'
      << ",\"symbol\":" << stop.symbol.value();
  if (always_expire || stop.expire_at != 0) {
    out << ",\"expire_at\":" << stop.expire_at;
  }
}

}  // namespace detail

inline Event parse_event_line(std::string_view line) {
  line = detail::trim(line);
  if (line.empty()) {
    throw std::runtime_error("empty event line");
  }

  const std::string type = detail::require_string(line, "type");
  if (type == "limit") {
    return detail::parse_limit_order(line);
  }
  if (type == "market") {
    return detail::parse_market_order(line);
  }
  if (type == "cancel") {
    return CancelOrder{
        .id = OrderId{static_cast<std::uint64_t>(detail::require_int(line, "id"))},
    };
  }
  if (type == "replace") {
    return ReplaceOrder{
        .id = OrderId{static_cast<std::uint64_t>(detail::require_int(line, "id"))},
        .price = Price{detail::require_int(line, "price")},
        .quantity = Quantity{static_cast<std::uint64_t>(detail::require_int(line, "quantity"))},
    };
  }
  if (type == "mass_cancel") {
    MassCancelFilter filter;
    if (detail::field(line, "account")) {
      filter.account = AccountId{static_cast<std::uint64_t>(detail::require_int(line, "account"))};
    }
    if (detail::field(line, "symbol")) {
      filter.symbol = Symbol{static_cast<std::uint64_t>(detail::require_int(line, "symbol"))};
    }
    if (detail::field(line, "side")) {
      filter.side = detail::parse_side(detail::require_string(line, "side"));
    }
    return MassCancelOrder{.filter = filter};
  }
  if (type == "stop") {
    return detail::parse_stop_order(line);
  }
  if (type == "reject") {
    const RiskDecision decision =
        detail::parse_decision(detail::require_string(line, "decision"));
    const std::string order_type = detail::require_string(line, "order_type");
    if (order_type == "limit") {
      return RejectEvent{.decision = decision, .attempt = detail::parse_limit_order(line)};
    }
    if (order_type == "market") {
      return RejectEvent{.decision = decision, .attempt = detail::parse_market_order(line)};
    }
    if (order_type == "stop") {
      return RejectEvent{.decision = decision, .attempt = detail::parse_stop_order(line)};
    }
    throw std::runtime_error("invalid reject order_type");
  }
  if (type == "time") {
    return TimeAdvance{
        .time = static_cast<std::uint64_t>(detail::require_int(line, "time")),
    };
  }
  throw std::runtime_error("unknown event type: " + type);
}

inline std::string format_event_line(const Event& event) {
  return std::visit(
      [](const auto& payload) -> std::string {
        using T = std::decay_t<decltype(payload)>;
        std::ostringstream out;
        if constexpr (std::is_same_v<T, Order>) {
          out << "{\"type\":\"limit\"";
          detail::append_limit_fields(out, payload);
          out << '}';
        } else if constexpr (std::is_same_v<T, MarketOrder>) {
          out << "{\"type\":\"market\"";
          detail::append_market_fields(out, payload);
          out << '}';
        } else if constexpr (std::is_same_v<T, CancelOrder>) {
          out << "{\"type\":\"cancel\",\"id\":" << payload.id.value() << '}';
        } else if constexpr (std::is_same_v<T, ReplaceOrder>) {
          out << "{\"type\":\"replace\""
              << ",\"id\":" << payload.id.value()
              << ",\"price\":" << payload.price.ticks()
              << ",\"quantity\":" << payload.quantity.value() << '}';
        } else if constexpr (std::is_same_v<T, MassCancelOrder>) {
          out << "{\"type\":\"mass_cancel\"";
          if (payload.filter.account) {
            out << ",\"account\":" << payload.filter.account->value();
          }
          if (payload.filter.symbol) {
            out << ",\"symbol\":" << payload.filter.symbol->value();
          }
          if (payload.filter.side) {
            out << ",\"side\":\"" << detail::format_side(*payload.filter.side) << '"';
          }
          out << '}';
        } else if constexpr (std::is_same_v<T, RejectEvent>) {
          out << "{\"type\":\"reject\""
              << ",\"decision\":\"" << detail::format_decision(payload.decision) << '"';
          std::visit(
              [&](const auto& attempt) {
                using A = std::decay_t<decltype(attempt)>;
                if constexpr (std::is_same_v<A, Order>) {
                  out << ",\"order_type\":\"limit\"";
                  detail::append_limit_fields(out, attempt);
                } else if constexpr (std::is_same_v<A, MarketOrder>) {
                  out << ",\"order_type\":\"market\"";
                  detail::append_market_fields(out, attempt);
                } else {
                  out << ",\"order_type\":\"stop\"";
                  detail::append_stop_fields(out, attempt);
                }
              },
              payload.attempt);
          out << '}';
        } else if constexpr (std::is_same_v<T, TimeAdvance>) {
          out << "{\"type\":\"time\",\"time\":" << payload.time << '}';
        } else {
          out << "{\"type\":\"stop\"";
          detail::append_stop_fields(out, payload);
          out << '}';
        }
        return out.str();
      },
      event);
}

inline EventLog load_event_log(std::istream& input) {
  EventLog log;
  std::string line;
  while (std::getline(input, line)) {
    if (detail::trim(line).empty()) {
      continue;
    }
    log.append(parse_event_line(line));
  }
  return log;
}

inline EventLog load_event_log(std::string_view text) {
  std::istringstream input{std::string(text)};
  return load_event_log(input);
}

inline EventLog load_event_log_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("failed to open event log: " + path.string());
  }
  return load_event_log(input);
}

inline void save_event_log(std::ostream& output, const EventLog& log) {
  for (const Event& event : log.events()) {
    output << format_event_line(event) << '\n';
  }
}

inline void save_event_log_file(const std::filesystem::path& path, const EventLog& log) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to write event log: " + path.string());
  }
  save_event_log(output, log);
}

}  // namespace jsonl
}  // namespace mercury
