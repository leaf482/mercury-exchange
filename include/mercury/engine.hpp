#pragma once

#include "mercury/balances.hpp"
#include "mercury/fees.hpp"
#include "mercury/order_book.hpp"
#include "mercury/positions.hpp"
#include "mercury/risk.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace mercury {

constexpr Side opposite_side(Side side) {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

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

// Per-symbol OrderBooks + shared Positions, with risk checks and stop orders.
class Engine {
 public:
  explicit Engine(RiskLimits limits = {},
                  SelfTradePrevention stp = SelfTradePrevention::Off,
                  FeeSchedule fees = {},
                  bool enforce_cash = false)
      : limits_(limits), stp_(stp), fees_(fees), enforce_cash_(enforce_cash) {}

  void set_enforce_cash(bool enabled) { enforce_cash_ = enabled; }

  bool enforce_cash() const { return enforce_cash_; }

  void set_cash(AccountId account, std::int64_t amount) {
    balances_.set_cash(account, amount);
  }

  std::int64_t cash(AccountId account) const { return balances_.cash(account); }

  std::int64_t reserved_cash(AccountId account) const {
    return balances_.reserved(account);
  }

  std::int64_t available_cash(AccountId account) const {
    return balances_.available(account);
  }

  std::uint64_t now() const { return now_; }

  // Advance discrete clock to `time` (no-op if time <= now). Cancels resting
  // orders / pending stops with expire_at <= now. Returns cancelled count.
  std::size_t advance_time(std::uint64_t time) {
    if (time <= now_) {
      return 0;
    }
    now_ = time;
    std::vector<OrderId> expired;
    for (const auto& [id, open] : open_orders_) {
      if (open.expire_at != 0 && open.expire_at <= now_) {
        expired.push_back(id);
      }
    }
    std::size_t cancelled = 0;
    for (OrderId id : expired) {
      if (cancel(id)) {
        ++cancelled;
      }
    }
    return cancelled;
  }

  SubmitResult add(Order order) {
    const Symbol symbol = order.symbol;
    auto result = submit_limit(std::move(order));
    if (result.decision != RiskDecision::Accept) {
      return result;
    }
    append_trades(result.trades, drain_stops(symbol));
    return result;
  }

  SubmitResult add_market(MarketOrder order) {
    const Symbol symbol = order.symbol;
    auto result = submit_market(std::move(order));
    if (result.decision != RiskDecision::Accept) {
      return result;
    }
    append_trades(result.trades, drain_stops(symbol));
    return result;
  }

  // Arms a stop. Triggers when last trade crosses stop_price (buy: >=, sell: <=).
  // Non-zero expire_at cancels the pending stop when the clock reaches it.
  SubmitResult add_stop(StopOrder stop) {
    const AccountId account = stop.account;
    const Symbol symbol = stop.symbol;
    const WorkingExposure exposure = working_for(account, symbol);
    const RiskDecision decision =
        check_order(limits_, positions_, account, stop.side, stop.quantity,
                    exposure.buy, exposure.sell, symbol);
    if (decision != RiskDecision::Accept) {
      return SubmitResult{.decision = decision};
    }
    if (stop.expire_at != 0 && stop.expire_at <= now_) {
      return SubmitResult{.decision = RiskDecision::InvalidExpire};
    }

    if (is_triggered(stop)) {
      return fire_stop(std::move(stop));
    }

    add_open(stop.id, symbol, account, stop.side, stop.quantity, Price{0},
             Quantity{0}, stop.expire_at);
    instrument(symbol).stops.push_back(std::move(stop));
    return SubmitResult{.decision = RiskDecision::Accept};
  }

  bool cancel(OrderId id) {
    const auto open_it = open_orders_.find(id);
    if (open_it == open_orders_.end()) {
      return false;
    }

    const Symbol symbol = open_it->second.symbol;
    Instrument& inst = instrument(symbol);

    if (erase_stop(inst, id)) {
      return true;
    }

    const Quantity remaining = open_it->second.remaining;
    reduce_open(id, remaining);
    return inst.book.cancel(id);
  }

  // Cancel-replace a resting GTC order (not a pending stop). Loses time priority.
  // quantity 0 cancels only. Returns nullopt if id is missing or is a stop.
  std::optional<SubmitResult> replace(OrderId id, Price price, Quantity quantity) {
    const auto open_it = open_orders_.find(id);
    if (open_it == open_orders_.end()) {
      return std::nullopt;
    }

    const Symbol symbol = open_it->second.symbol;
    Instrument& inst = instrument(symbol);
    for (const StopOrder& stop : inst.stops) {
      if (stop.id == id) {
        return std::nullopt;
      }
    }

    const OpenOrder open = open_it->second;
    if (!cancel(id)) {
      return std::nullopt;
    }
    if (quantity.is_zero()) {
      return SubmitResult{.decision = RiskDecision::Accept};
    }

    return add(Order{
        .id = id,
        .side = open.side,
        .price = price,
        .quantity = quantity,
        .account = open.account,
        .tif = open.expire_at != 0 ? TimeInForce::Gtd : TimeInForce::Gtc,
        .symbol = symbol,
        .display = open.display,
        .expire_at = open.expire_at,
    });
  }

  // Cancel resting orders and pending stops matching all set filter fields.
  std::size_t mass_cancel(MassCancelFilter filter = {}) {
    std::vector<OrderId> ids;
    ids.reserve(open_orders_.size());
    for (const auto& [id, open] : open_orders_) {
      if (filter.account && open.account != *filter.account) {
        continue;
      }
      if (filter.symbol && open.symbol != *filter.symbol) {
        continue;
      }
      if (filter.side && open.side != *filter.side) {
        continue;
      }
      ids.push_back(id);
    }

    std::size_t cancelled = 0;
    for (const OrderId id : ids) {
      if (cancel(id)) {
        ++cancelled;
      }
    }
    return cancelled;
  }

  BookSnapshot snapshot(std::size_t max_levels, Symbol symbol = Symbol{0}) const {
    const Instrument* inst = find_instrument(symbol);
    return inst ? inst->book.snapshot(max_levels) : BookSnapshot{};
  }

  const OrderBook& book(Symbol symbol = Symbol{0}) const {
    const Instrument* inst = find_instrument(symbol);
    if (inst) {
      return inst->book;
    }
    static const OrderBook empty;
    return empty;
  }

  const Positions& positions() const { return positions_; }

  const FeeSchedule& fees() const { return fees_; }

  // Cumulative fees paid by account (positive = paid; negative = rebate received).
  std::int64_t fees_paid(AccountId account) const {
    const auto it = fees_paid_.find(account);
    return it == fees_paid_.end() ? 0 : it->second;
  }

  std::optional<Price> last_trade_price(Symbol symbol = Symbol{0}) const {
    const Instrument* inst = find_instrument(symbol);
    return inst ? inst->last_trade_price : std::nullopt;
  }

  // Mid = (best_bid + best_ask) / 2 in ticks; nullopt if either side is missing.
  std::optional<Price> mid_price(Symbol symbol = Symbol{0}) const {
    const auto bid = book(symbol).best_bid();
    const auto ask = book(symbol).best_ask();
    if (!bid || !ask) {
      return std::nullopt;
    }
    return Price{(bid->ticks() + ask->ticks()) / 2};
  }

  std::optional<Price> mark_price(MarkSource source = MarkSource::LastTrade,
                                  Symbol symbol = Symbol{0}) const {
    switch (source) {
      case MarkSource::LastTrade:
        return last_trade_price(symbol);
      case MarkSource::Mid:
        return mid_price(symbol);
    }
    return std::nullopt;
  }

  std::int64_t realized_pnl(AccountId account, Symbol symbol = Symbol{0}) const {
    return positions_.realized_pnl(account, symbol);
  }

  // Default mark is last trade; Mid needs a two-sided book.
  std::optional<std::int64_t> unrealized_pnl(
      AccountId account, Symbol symbol = Symbol{0},
      MarkSource mark = MarkSource::LastTrade) const {
    const auto price = mark_price(mark, symbol);
    if (!price) {
      return std::nullopt;
    }
    return positions_.unrealized_pnl(account, *price, symbol);
  }

  AccountReport account_report(AccountId account,
                               MarkSource mark = MarkSource::LastTrade) const {
    AccountReport report;
    report.account = account;
    report.cash = balances_.cash(account);
    report.reserved = balances_.reserved(account);
    report.available = balances_.available(account);
    report.fees_paid = fees_paid(account);

    bool missing_mark = false;
    std::int64_t unrealized_sum = 0;
    for (const auto& entry : positions_.for_account(account)) {
      PositionReport row{
          .symbol = entry.symbol,
          .quantity = entry.quantity,
          .avg_ticks = entry.avg_ticks,
          .realized_pnl = entry.realized_pnl,
          .mark_ticks = std::nullopt,
          .unrealized_pnl = std::nullopt,
      };
      report.realized_pnl += entry.realized_pnl;

      if (entry.quantity != 0) {
        const auto price = mark_price(mark, entry.symbol);
        if (price) {
          row.mark_ticks = price->ticks();
          row.unrealized_pnl =
              positions_.unrealized_pnl(account, *price, entry.symbol);
          unrealized_sum += *row.unrealized_pnl;
          report.inventory_mark += entry.quantity * price->ticks();
        } else {
          missing_mark = true;
        }
      }
      report.positions.push_back(row);
    }

    if (!missing_mark) {
      report.unrealized_pnl = unrealized_sum;
    }
    report.equity = report.cash + report.inventory_mark;
    return report;
  }

  std::size_t pending_stop_count(Symbol symbol = Symbol{0}) const {
    const Instrument* inst = find_instrument(symbol);
    return inst ? inst->stops.size() : 0;
  }

  // Next id that will be assigned (starts at 1).
  TradeId next_trade_id() const { return TradeId{next_trade_id_}; }

  RestartSnapshot restart_snapshot() const {
    RestartSnapshot snap;
    snap.now = now_;
    snap.next_trade_id = next_trade_id_;
    snap.enforce_cash = enforce_cash_;
    snap.fees = fees_;
    snap.stp = stp_;
    snap.limits = limits_;
    for (const auto& [account, amount] : balances_.cash_entries()) {
      snap.cash.push_back(RestartCash{.account = account, .amount = amount});
    }
    for (const auto& [account, amount] : fees_paid_) {
      if (amount != 0) {
        snap.fees_paid.push_back(RestartFees{.account = account, .amount = amount});
      }
    }
    for (const auto& row : positions_.entries()) {
      snap.positions.push_back(RestartPosition{
          .account = row.account,
          .symbol = row.symbol,
          .quantity = row.quantity,
          .avg_ticks = row.avg_ticks,
          .realized_pnl = row.realized_pnl,
      });
    }
    for (const auto& [symbol, inst] : instruments_) {
      if (inst.last_trade_price) {
        snap.last_trades.push_back(
            RestartLastTrade{.symbol = symbol, .price = *inst.last_trade_price});
      }
      for (const Order& order : inst.book.resting_orders()) {
        Quantity reserve{0};
        const auto open = open_orders_.find(order.id);
        if (open != open_orders_.end()) {
          reserve = open->second.cash_reserve_qty;
        }
        snap.orders.push_back(RestartOrder{.order = order, .cash_reserve_qty = reserve});
      }
      for (const StopOrder& stop : inst.stops) {
        snap.stops.push_back(stop);
      }
    }
    return snap;
  }

  // Replaces matching state. Caller-supplied config in `snap` wins.
  void load_restart(const RestartSnapshot& snap) {
    limits_ = snap.limits;
    stp_ = snap.stp;
    fees_ = snap.fees;
    enforce_cash_ = snap.enforce_cash;
    now_ = snap.now;
    next_trade_id_ = snap.next_trade_id;
    balances_ = Balances{};
    instruments_.clear();
    positions_ = Positions{};
    open_orders_.clear();
    working_.clear();
    fees_paid_.clear();

    for (const RestartCash& row : snap.cash) {
      balances_.set_cash(row.account, row.amount);
    }
    for (const RestartFees& row : snap.fees_paid) {
      fees_paid_[row.account] = row.amount;
    }
    for (const RestartPosition& row : snap.positions) {
      positions_.assign(row.account, row.symbol, row.quantity, row.avg_ticks,
                        row.realized_pnl);
    }
    for (const RestartLastTrade& row : snap.last_trades) {
      instrument(row.symbol).last_trade_price = row.price;
    }
    for (const RestartOrder& row : snap.orders) {
      const Order& order = row.order;
      instrument(order.symbol).book.restore(order);
      add_open(order.id, order.symbol, order.account, order.side, order.quantity,
               order.price, order.display, order.expire_at, row.cash_reserve_qty);
    }
    for (const StopOrder& stop : snap.stops) {
      add_open(stop.id, stop.symbol, stop.account, stop.side, stop.quantity, Price{0},
               Quantity{0}, stop.expire_at);
      instrument(stop.symbol).stops.push_back(stop);
    }
  }

 private:
  struct WorkingExposure {
    std::uint64_t buy = 0;
    std::uint64_t sell = 0;
  };

  struct OpenOrder {
    Symbol symbol;
    AccountId account;
    Side side;
    Quantity remaining;
    Price price{0};  // set for book rests (cash reservation); 0 for pending stops
    Quantity display{0};
    std::uint64_t expire_at{0};
    // Qty still covered by cash reservation (full buys; short portion of sells).
    Quantity cash_reserve_qty{0};
  };

  struct Instrument {
    explicit Instrument(SelfTradePrevention stp = SelfTradePrevention::Off)
        : book(stp) {}

    OrderBook book;
    std::vector<StopOrder> stops;
    std::optional<Price> last_trade_price;
  };

  Instrument& instrument(Symbol symbol) {
    const auto [it, inserted] = instruments_.try_emplace(symbol, stp_);
    (void)inserted;
    return it->second;
  }

  const Instrument* find_instrument(Symbol symbol) const {
    const auto it = instruments_.find(symbol);
    return it == instruments_.end() ? nullptr : &it->second;
  }

  WorkingExposure working_for(AccountId account, Symbol symbol) const {
    const auto it = working_.find({account, symbol});
    return it == working_.end() ? WorkingExposure{} : it->second;
  }

  void add_open(OrderId id, Symbol symbol, AccountId account, Side side,
                Quantity quantity, Price price = Price{0},
                Quantity display = Quantity{0}, std::uint64_t expire_at = 0,
                Quantity cash_reserve_qty = Quantity{0}) {
    open_orders_.insert_or_assign(
        id, OpenOrder{symbol, account, side, quantity, price, display, expire_at,
                      cash_reserve_qty});
    WorkingExposure& exposure = working_[{account, symbol}];
    if (side == Side::Buy) {
      exposure.buy += quantity.value();
    } else {
      exposure.sell += quantity.value();
    }
    if (enforce_cash_ && price.ticks() != 0 && !cash_reserve_qty.is_zero()) {
      balances_.reserve(
          account,
          price.ticks() * static_cast<std::int64_t>(cash_reserve_qty.value()));
    }
  }

  void reduce_open(OrderId id, Quantity fill) {
    const auto it = open_orders_.find(id);
    if (it == open_orders_.end()) {
      return;
    }

    OpenOrder& open = it->second;
    WorkingExposure& exposure = working_[{open.account, open.symbol}];
    if (open.side == Side::Buy) {
      exposure.buy -= fill.value();
    } else {
      exposure.sell -= fill.value();
    }
    if (enforce_cash_ && open.price.ticks() != 0 && !open.cash_reserve_qty.is_zero()) {
      const Quantity release_qty{
          std::min(fill.value(), open.cash_reserve_qty.value())};
      balances_.release(
          open.account,
          open.price.ticks() * static_cast<std::int64_t>(release_qty.value()));
      open.cash_reserve_qty = open.cash_reserve_qty - release_qty;
    }

    open.remaining = open.remaining - fill;
    if (open.remaining.is_zero()) {
      open_orders_.erase(it);
    }
  }

  void apply_trades(Symbol symbol, Side taker_side, std::vector<Trade>& trades) {
    Instrument& inst = instrument(symbol);
    const Side maker_side = opposite_side(taker_side);
    for (Trade& trade : trades) {
      trade.id = TradeId{next_trade_id_++};
      const std::int64_t notional =
          trade.price.ticks() * static_cast<std::int64_t>(trade.quantity.value());
      trade.maker_fee = fee_from_notional(notional, fees_.maker_bps);
      trade.taker_fee = fee_from_notional(notional, fees_.taker_bps);
      fees_paid_[trade.maker_account] += trade.maker_fee;
      fees_paid_[trade.taker_account] += trade.taker_fee;

      // Buyers pay notional; sellers receive notional; both pay their fee.
      if (taker_side == Side::Buy) {
        balances_.adjust(trade.taker_account, -(notional + trade.taker_fee));
        balances_.adjust(trade.maker_account, notional - trade.maker_fee);
      } else {
        balances_.adjust(trade.taker_account, notional - trade.taker_fee);
        balances_.adjust(trade.maker_account, -(notional + trade.maker_fee));
      }

      positions_.fill(trade.taker_account, taker_side, trade.price, trade.quantity,
                      symbol);
      positions_.fill(trade.maker_account, maker_side, trade.price, trade.quantity,
                      symbol);
      inst.last_trade_price = trade.price;
    }
  }

  static void append_trades(std::vector<Trade>& into, std::vector<Trade> extra) {
    into.insert(into.end(), extra.begin(), extra.end());
  }

  bool is_triggered(const StopOrder& stop) const {
    const Instrument* inst = find_instrument(stop.symbol);
    if (!inst || !inst->last_trade_price) {
      return false;
    }
    if (stop.side == Side::Buy) {
      return inst->last_trade_price->ticks() >= stop.stop_price.ticks();
    }
    return inst->last_trade_price->ticks() <= stop.stop_price.ticks();
  }

  bool erase_stop(Instrument& inst, OrderId id) {
    for (auto it = inst.stops.begin(); it != inst.stops.end(); ++it) {
      if (it->id == id) {
        reduce_open(id, it->quantity);
        inst.stops.erase(it);
        return true;
      }
    }
    return false;
  }

  SubmitResult fire_stop(StopOrder stop) {
    SubmitResult result;
    const Symbol symbol = stop.symbol;
    if (stop.limit_price) {
      result = submit_limit(Order{
          .id = stop.id,
          .side = stop.side,
          .price = *stop.limit_price,
          .quantity = stop.quantity,
          .account = stop.account,
          .tif = stop.tif,
          .symbol = symbol,
          .expire_at = stop.tif == TimeInForce::Gtd ? stop.expire_at : 0,
      });
    } else {
      result = submit_market(MarketOrder{
          .id = stop.id,
          .side = stop.side,
          .quantity = stop.quantity,
          .account = stop.account,
          .symbol = symbol,
      });
    }
    if (result.decision == RiskDecision::Accept) {
      append_trades(result.trades, drain_stops(symbol));
    }
    return result;
  }

  std::vector<Trade> drain_stops(Symbol symbol) {
    std::vector<Trade> trades;
    Instrument& inst = instrument(symbol);
    bool progressed = true;
    while (progressed) {
      progressed = false;
      for (auto it = inst.stops.begin(); it != inst.stops.end();) {
        if (!is_triggered(*it)) {
          ++it;
          continue;
        }

        StopOrder stop = std::move(*it);
        it = inst.stops.erase(it);
        reduce_open(stop.id, stop.quantity);

        SubmitResult fired;
        if (stop.limit_price) {
          fired = submit_limit(Order{
              .id = stop.id,
              .side = stop.side,
              .price = *stop.limit_price,
              .quantity = stop.quantity,
              .account = stop.account,
              .tif = stop.tif,
              .symbol = symbol,
              .expire_at = stop.tif == TimeInForce::Gtd ? stop.expire_at : 0,
          });
        } else {
          fired = submit_market(MarketOrder{
              .id = stop.id,
              .side = stop.side,
              .quantity = stop.quantity,
              .account = stop.account,
              .symbol = symbol,
          });
        }

        if (fired.decision == RiskDecision::Accept) {
          append_trades(trades, std::move(fired.trades));
          progressed = true;
        }
        break;  // restart scan; vector invalidated / price may have moved
      }
    }
    return trades;
  }

  SubmitResult submit_limit(Order order) {
    const AccountId account = order.account;
    const Symbol symbol = order.symbol;
    const WorkingExposure exposure = working_for(account, symbol);
    const RiskDecision decision =
        check_order(limits_, positions_, account, order.side, order.quantity,
                    exposure.buy, exposure.sell, symbol);
    if (decision != RiskDecision::Accept) {
      return SubmitResult{.decision = decision};
    }
    if (order.tif == TimeInForce::Gtd) {
      if (order.expire_at == 0 || order.expire_at <= now_) {
        return SubmitResult{.decision = RiskDecision::InvalidExpire};
      }
    } else {
      order.expire_at = 0;
    }
    if (order.reduce_only) {
      const RiskDecision reduce =
          check_reduce_only(positions_, account, order.side, order.quantity, symbol);
      if (reduce != RiskDecision::Accept) {
        return SubmitResult{.decision = reduce};
      }
    }
    if (order.post_only && instrument(symbol).book.would_take(order)) {
      return SubmitResult{.decision = RiskDecision::PostOnly};
    }
    if (order.side == Side::Buy) {
      const RiskDecision cash =
          check_buy_cash(account, symbol, order.price, order.quantity, false);
      if (cash != RiskDecision::Accept) {
        return SubmitResult{.decision = cash};
      }
    } else {
      const RiskDecision cash =
          check_sell_margin(account, symbol, order.price, order.quantity, false);
      if (cash != RiskDecision::Accept) {
        return SubmitResult{.decision = cash};
      }
    }

    const OrderId id = order.id;
    const Side taker_side = order.side;
    const Quantity original = order.quantity;
    const TimeInForce tif = order.tif;
    const Price order_price = order.price;
    const Quantity display = order.display;
    const std::uint64_t expire_at = order.expire_at;
    auto trades = instrument(symbol).book.add(std::move(order));
    clear_stp_cancels(symbol);

    Quantity filled{0};
    for (const Trade& trade : trades) {
      filled = Quantity{filled.value() + trade.quantity.value()};
      reduce_open(trade.maker_id, trade.quantity);
    }

    const Quantity rested{original.value() - filled.value()};
    if (rests_on_book(tif) && !rested.is_zero()) {
      Quantity reserve_qty{0};
      if (enforce_cash_ && order_price.ticks() != 0) {
        if (taker_side == Side::Buy) {
          reserve_qty = rested;
        } else {
          const std::uint64_t cover =
              free_long(account, symbol, exposure.sell).value();
          const std::uint64_t covered_fill = std::min(filled.value(), cover);
          const std::uint64_t cover_left = cover - covered_fill;
          const std::uint64_t short_rest =
              rested.value() > cover_left ? rested.value() - cover_left : 0;
          reserve_qty = Quantity{short_rest};
        }
      }
      add_open(id, symbol, account, taker_side, rested, order_price, display,
               expire_at, reserve_qty);
    }

    apply_trades(symbol, taker_side, trades);
    return SubmitResult{.decision = RiskDecision::Accept, .trades = std::move(trades)};
  }

  SubmitResult submit_market(MarketOrder order) {
    const AccountId account = order.account;
    const Symbol symbol = order.symbol;
    const WorkingExposure exposure = working_for(account, symbol);
    const RiskDecision decision =
        check_order(limits_, positions_, account, order.side, order.quantity,
                    exposure.buy, exposure.sell, symbol);
    if (decision != RiskDecision::Accept) {
      return SubmitResult{.decision = decision};
    }
    if (order.reduce_only) {
      const RiskDecision reduce =
          check_reduce_only(positions_, account, order.side, order.quantity, symbol);
      if (reduce != RiskDecision::Accept) {
        return SubmitResult{.decision = reduce};
      }
    }
    if (order.side == Side::Buy) {
      const RiskDecision cash =
          check_buy_cash(account, symbol, Price{0}, order.quantity, true);
      if (cash != RiskDecision::Accept) {
        return SubmitResult{.decision = cash};
      }
    } else {
      const RiskDecision cash =
          check_sell_margin(account, symbol, Price{0}, order.quantity, true);
      if (cash != RiskDecision::Accept) {
        return SubmitResult{.decision = cash};
      }
    }

    const Side taker_side = order.side;
    auto trades = instrument(symbol).book.add_market(std::move(order));
    clear_stp_cancels(symbol);
    for (const Trade& trade : trades) {
      reduce_open(trade.maker_id, trade.quantity);
    }
    apply_trades(symbol, taker_side, trades);
    return SubmitResult{.decision = RiskDecision::Accept, .trades = std::move(trades)};
  }

  void clear_stp_cancels(Symbol symbol) {
    auto cancels = instrument(symbol).book.take_stp_cancels();
    for (const OrderId id : cancels) {
      const auto it = open_orders_.find(id);
      if (it != open_orders_.end()) {
        reduce_open(id, it->second.remaining);
      }
    }
  }

  // Worst-case buy cost at limit price, or walking asks for market (available liquidity only).
  RiskDecision check_buy_cash(AccountId account, Symbol symbol, Price limit,
                              Quantity quantity, bool is_market) const {
    if (!enforce_cash_) {
      return RiskDecision::Accept;
    }

    std::int64_t need = 0;
    if (!is_market) {
      need = limit.ticks() * static_cast<std::int64_t>(quantity.value());
    } else {
      need = book(symbol).estimate_buy_notional(quantity, true);
    }

    if (balances_.available(account) < need) {
      return RiskDecision::InsufficientCash;
    }
    return RiskDecision::Accept;
  }

  Quantity free_long(AccountId account, Symbol symbol,
                     std::uint64_t working_sell) const {
    const std::int64_t pos = positions_.quantity(account, symbol);
    const std::int64_t long_qty = pos > 0 ? pos : 0;
    const std::int64_t free = long_qty - static_cast<std::int64_t>(working_sell);
    return Quantity{static_cast<std::uint64_t>(free > 0 ? free : 0)};
  }

  Quantity short_qty(AccountId account, Symbol symbol, Quantity quantity,
                     std::uint64_t working_sell) const {
    const std::uint64_t cover = free_long(account, symbol, working_sell).value();
    if (quantity.value() <= cover) {
      return Quantity{0};
    }
    return Quantity{quantity.value() - cover};
  }

  // Margin for the uncovered (short) portion of a sell.
  RiskDecision check_sell_margin(AccountId account, Symbol symbol, Price limit,
                                 Quantity quantity, bool is_market) const {
    if (!enforce_cash_) {
      return RiskDecision::Accept;
    }
    const WorkingExposure exposure = working_for(account, symbol);
    const Quantity uncovered = short_qty(account, symbol, quantity, exposure.sell);
    if (uncovered.is_zero()) {
      return RiskDecision::Accept;
    }

    std::int64_t need = 0;
    if (!is_market) {
      need = limit.ticks() * static_cast<std::int64_t>(uncovered.value());
    } else {
      need = book(symbol).estimate_sell_notional(uncovered, true);
    }

    if (balances_.available(account) < need) {
      return RiskDecision::InsufficientCash;
    }
    return RiskDecision::Accept;
  }

  RiskLimits limits_;
  SelfTradePrevention stp_;
  FeeSchedule fees_;
  bool enforce_cash_{false};
  std::uint64_t now_{0};
  Balances balances_;
  std::map<Symbol, Instrument> instruments_;
  Positions positions_;
  std::map<OrderId, OpenOrder> open_orders_;
  std::map<std::pair<AccountId, Symbol>, WorkingExposure> working_;
  std::map<AccountId, std::int64_t> fees_paid_;
  std::uint64_t next_trade_id_{1};
};

}  // namespace mercury
