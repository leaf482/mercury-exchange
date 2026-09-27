#pragma once

#include "mercury/types.hpp"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace mercury {

// Cash ledger in tick * quantity units (same as notional / fees).
class Balances {
 public:
  std::int64_t cash(AccountId account) const {
    const auto it = cash_.find(account);
    return it == cash_.end() ? 0 : it->second;
  }

  std::int64_t reserved(AccountId account) const {
    const auto it = reserved_.find(account);
    return it == reserved_.end() ? 0 : it->second;
  }

  std::int64_t available(AccountId account) const {
    return cash(account) - reserved(account);
  }

  void set_cash(AccountId account, std::int64_t amount) { cash_[account] = amount; }

  void adjust(AccountId account, std::int64_t delta) { cash_[account] += delta; }

  void reserve(AccountId account, std::int64_t amount) {
    if (amount != 0) {
      reserved_[account] += amount;
    }
  }

  std::vector<std::pair<AccountId, std::int64_t>> cash_entries() const {
    std::vector<std::pair<AccountId, std::int64_t>> out;
    for (const auto& [account, amount] : cash_) {
      if (amount != 0) {
        out.emplace_back(account, amount);
      }
    }
    return out;
  }

  void release(AccountId account, std::int64_t amount) {
    if (amount == 0) {
      return;
    }
    std::int64_t& held = reserved_[account];
    held -= amount;
    if (held <= 0) {
      reserved_.erase(account);
    }
  }

 private:
  std::map<AccountId, std::int64_t> cash_;
  std::map<AccountId, std::int64_t> reserved_;
};

}  // namespace mercury
