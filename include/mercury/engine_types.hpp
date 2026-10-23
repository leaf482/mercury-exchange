#pragma once

#include "mercury/fees.hpp"
#include "mercury/order.hpp"
#include "mercury/risk.hpp"
#include "mercury/trade.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace mercury {

struct SubmitResult {
  RiskDecision decision{RiskDecision::Accept};
  std::vector<Trade> trades{};
};

enum class MarkSource : std::uint8_t { LastTrade, Mid };

struct MassCancelFilter {
  std::optional<AccountId> account;
  std::optional<Symbol> symbol;
  std::optional<Side> side;

  constexpr bool operator==(const MassCancelFilter&) const = default;
};

struct PositionReport {
  Symbol symbol{0};
  std::int64_t quantity{0};
  std::int64_t avg_ticks{0};
  std::int64_t realized_pnl{0};
  std::optional<std::int64_t> mark_ticks;
  std::optional<std::int64_t> unrealized_pnl;
};

// Portfolio view for one account (tick * quantity cash units).
struct AccountReport {
  AccountId account{0};
  std::int64_t cash{0};
  std::int64_t reserved{0};
  std::int64_t available{0};
  std::int64_t fees_paid{0};
  std::vector<PositionReport> positions;
  std::int64_t realized_pnl{0};
  std::optional<std::int64_t> unrealized_pnl;  // nullopt if any open row lacks mark
  std::int64_t inventory_mark{0};              // sum(qty * mark) where mark known
  std::int64_t equity{0};                      // cash + inventory_mark
};

struct RestartCash {
  AccountId account{0};
  std::int64_t amount{0};
};

struct RestartFees {
  AccountId account{0};
  std::int64_t amount{0};
};

struct RestartPosition {
  AccountId account{0};
  Symbol symbol{0};
  std::int64_t quantity{0};
  std::int64_t avg_ticks{0};
  std::int64_t realized_pnl{0};
};

struct RestartLastTrade {
  Symbol symbol{0};
  Price price{0};
};

struct RestartOrder {
  Order order;
  Quantity cash_reserve_qty{0};
};

// Enough state to continue matching on a fresh Engine. Not a database: one
// in-memory copy (and a JSONL encoding) of book, positions, cash, and clock.
struct RestartSnapshot {
  std::uint64_t now{0};
  std::uint64_t next_trade_id{1};
  bool enforce_cash{false};
  FeeSchedule fees{};
  SelfTradePrevention stp{SelfTradePrevention::Off};
  RiskLimits limits{};
  std::vector<RestartCash> cash;
  std::vector<RestartFees> fees_paid;
  std::vector<RestartPosition> positions;
  std::vector<RestartLastTrade> last_trades;
  std::vector<RestartOrder> orders;
  std::vector<StopOrder> stops;
};

}  // namespace mercury
