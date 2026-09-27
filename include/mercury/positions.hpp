#pragma once

#include "mercury/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace mercury {

// Tracks signed position and PnL in tick * quantity units, per (account, symbol).
class Positions {
 public:
  void fill(AccountId account, Side side, Price price, Quantity quantity,
            Symbol symbol = Symbol{0}) {
    AccountState& state = accounts_[{account, symbol}];
    const std::int64_t fill_qty = static_cast<std::int64_t>(quantity.value());
    const std::int64_t px = price.ticks();
    const std::int64_t delta = (side == Side::Buy) ? fill_qty : -fill_qty;

    if (state.qty == 0 || same_sign(state.qty, delta)) {
      const std::int64_t abs_old = std::abs(state.qty);
      const std::int64_t abs_add = std::abs(delta);
      state.avg_ticks =
          (abs_old * state.avg_ticks + abs_add * px) / (abs_old + abs_add);
      state.qty += delta;
      return;
    }

    const std::int64_t close_qty = std::min(std::abs(delta), std::abs(state.qty));
    if (state.qty > 0) {
      state.realized += (px - state.avg_ticks) * close_qty;
    } else {
      state.realized += (state.avg_ticks - px) * close_qty;
    }

    const std::int64_t previous = state.qty;
    state.qty += delta;

    if (state.qty == 0) {
      state.avg_ticks = 0;
    } else if (!same_sign(previous, state.qty)) {
      state.avg_ticks = px;
    }
  }

  std::int64_t quantity(AccountId account, Symbol symbol = Symbol{0}) const {
    const auto it = accounts_.find({account, symbol});
    return it == accounts_.end() ? 0 : it->second.qty;
  }

  // Cumulative realized PnL in tick * quantity units.
  std::int64_t realized_pnl(AccountId account, Symbol symbol = Symbol{0}) const {
    const auto it = accounts_.find({account, symbol});
    return it == accounts_.end() ? 0 : it->second.realized;
  }

  // Unrealized PnL at mark, in tick * quantity units.
  std::int64_t unrealized_pnl(AccountId account, Price mark,
                              Symbol symbol = Symbol{0}) const {
    const auto it = accounts_.find({account, symbol});
    if (it == accounts_.end() || it->second.qty == 0) {
      return 0;
    }
    return (mark.ticks() - it->second.avg_ticks) * it->second.qty;
  }

  std::int64_t avg_ticks(AccountId account, Symbol symbol = Symbol{0}) const {
    const auto it = accounts_.find({account, symbol});
    return it == accounts_.end() ? 0 : it->second.avg_ticks;
  }

  struct Entry {
    Symbol symbol;
    std::int64_t quantity = 0;
    std::int64_t avg_ticks = 0;
    std::int64_t realized_pnl = 0;
  };

  void assign(AccountId account, Symbol symbol, std::int64_t quantity,
              std::int64_t avg_ticks, std::int64_t realized_pnl) {
    if (quantity == 0 && realized_pnl == 0 && avg_ticks == 0) {
      return;
    }
    accounts_[{account, symbol}] =
        AccountState{.qty = quantity, .avg_ticks = avg_ticks, .realized = realized_pnl};
  }

  struct Stored {
    AccountId account;
    Symbol symbol;
    std::int64_t quantity = 0;
    std::int64_t avg_ticks = 0;
    std::int64_t realized_pnl = 0;
  };

  std::vector<Stored> entries() const {
    std::vector<Stored> out;
    for (const auto& [key, state] : accounts_) {
      if (state.qty == 0 && state.realized == 0) {
        continue;
      }
      out.push_back(Stored{
          .account = key.first,
          .symbol = key.second,
          .quantity = state.qty,
          .avg_ticks = state.avg_ticks,
          .realized_pnl = state.realized,
      });
    }
    return out;
  }

  // Non-flat or realized-nonzero rows for one account.
  std::vector<Entry> for_account(AccountId account) const {
    std::vector<Entry> out;
    for (const auto& [key, state] : accounts_) {
      if (key.first != account) {
        continue;
      }
      if (state.qty == 0 && state.realized == 0) {
        continue;
      }
      out.push_back(Entry{
          .symbol = key.second,
          .quantity = state.qty,
          .avg_ticks = state.avg_ticks,
          .realized_pnl = state.realized,
      });
    }
    return out;
  }

 private:
  struct AccountState {
    std::int64_t qty = 0;
    std::int64_t avg_ticks = 0;
    std::int64_t realized = 0;
  };

  static bool same_sign(std::int64_t a, std::int64_t b) {
    return (a > 0 && b > 0) || (a < 0 && b < 0);
  }

  std::map<std::pair<AccountId, Symbol>, AccountState> accounts_;
};

}  // namespace mercury
