from __future__ import annotations

from dataclasses import dataclass
from typing import Literal, Optional

from mercury_sim.book import Order, OrderBook, Trade
from mercury_sim.events import (
    Event,
    LimitEvent,
    MarketEvent,
    MassCancelEvent,
    RejectEvent,
    ReplaceEvent,
    StopEvent,
    TimeEvent,
)


@dataclass
class _PendingStop:
    event: StopEvent


class Engine:
    """Python twin of C++ Engine: per-symbol books + last-trade stop triggers."""

    def __init__(
        self,
        stp: Literal["off", "cancel_resting"] = "off",
        maker_bps: int = 0,
        taker_bps: int = 0,
        enforce_cash: bool = False,
    ) -> None:
        self._stp = stp
        self._maker_bps = maker_bps
        self._taker_bps = taker_bps
        self._enforce_cash = enforce_cash
        self._books: dict[int, OrderBook] = {}
        self._stops: dict[int, list[_PendingStop]] = {}
        self._last_trade: dict[int, int] = {}
        self._stop_index: dict[int, int] = {}  # order id -> symbol
        self._fees_paid: dict[int, int] = {}
        self._positions: dict[tuple[int, int], int] = {}
        self._avg_ticks: dict[tuple[int, int], int] = {}
        self._realized: dict[tuple[int, int], int] = {}
        self._next_trade_id = 1
        self._cash: dict[int, int] = {}
        self._reserved: dict[int, int] = {}
        # Resting cash reservations: order_id -> (account, price, reserve_qty)
        self._cash_rests: dict[int, tuple[int, int, int]] = {}
        # GTD deadlines for resting orders: order_id -> expire_at
        self._expire_at: dict[int, int] = {}
        self._now = 0

    def book(self, symbol: int = 0) -> OrderBook:
        return self._books.setdefault(symbol, OrderBook(stp=self._stp))

    def cash(self, account: int) -> int:
        return self._cash.get(account, 0)

    def reserved_cash(self, account: int) -> int:
        return self._reserved.get(account, 0)

    def available_cash(self, account: int) -> int:
        return self.cash(account) - self.reserved_cash(account)

    def set_cash(self, account: int, amount: int) -> None:
        self._cash[account] = amount

    def set_enforce_cash(self, enabled: bool) -> None:
        self._enforce_cash = enabled

    def now(self) -> int:
        return self._now

    def advance_time(self, time: int) -> int:
        if time <= self._now:
            return 0
        self._now = time
        expired = [oid for oid, exp in list(self._expire_at.items()) if exp <= self._now]
        cancelled = 0
        for order_id in expired:
            if self.cancel(order_id):
                cancelled += 1
        return cancelled

    def position(self, account: int, symbol: int = 0) -> int:
        return self._positions.get((account, symbol), 0)

    def next_trade_id(self) -> int:
        return self._next_trade_id

    def enforce_cash(self) -> bool:
        return self._enforce_cash

    def maker_bps(self) -> int:
        return self._maker_bps

    def taker_bps(self) -> int:
        return self._taker_bps

    def stp(self) -> str:
        return self._stp

    def cash_balances(self) -> list[tuple[int, int]]:
        return sorted(self._cash.items())

    def fees_balances(self) -> list[tuple[int, int]]:
        return sorted(self._fees_paid.items())

    def position_rows(self) -> list[dict]:
        keys = set(self._positions) | set(self._realized) | set(self._avg_ticks)
        rows: list[dict] = []
        for account, symbol in keys:
            quantity = self.position(account, symbol)
            realized = self.realized_pnl(account, symbol)
            if quantity == 0 and realized == 0:
                continue
            rows.append(
                {
                    "account": account,
                    "symbol": symbol,
                    "quantity": quantity,
                    "avg_ticks": self._avg_ticks.get((account, symbol), 0),
                    "realized_pnl": realized,
                }
            )
        rows.sort(key=lambda row: (row["account"], row["symbol"]))
        return rows

    def last_trades(self) -> list[tuple[int, int]]:
        return sorted(self._last_trade.items())

    def resting_rows(self) -> list[tuple[Order, int]]:
        symbols = set(self._books)
        rows: list[tuple[Order, int]] = []
        for symbol in sorted(symbols):
            for order in self.book(symbol).resting_orders():
                rest = self._cash_rests.get(order.id)
                reserve = rest[2] if rest is not None else 0
                rows.append((order, reserve))
        return rows

    def pending_stops(self) -> list[StopEvent]:
        stops: list[StopEvent] = []
        for symbol in sorted(self._stops):
            for item in self._stops[symbol]:
                stops.append(item.event)
        return stops

    def load_restart_records(self, records: list[dict]) -> None:
        from mercury_sim.events import StopEvent as _Stop

        meta = records[0]
        self._now = int(meta["now"])
        self._next_trade_id = int(meta["next_trade_id"])
        for rec in records[1:]:
            kind = rec["type"]
            if kind == "cash":
                self._cash[int(rec["account"])] = int(rec["amount"])
            elif kind == "fees_paid":
                self._fees_paid[int(rec["account"])] = int(rec["amount"])
            elif kind == "position":
                key = (int(rec["account"]), int(rec["symbol"]))
                self._positions[key] = int(rec["quantity"])
                self._avg_ticks[key] = int(rec["avg_ticks"])
                self._realized[key] = int(rec["realized_pnl"])
            elif kind == "last_trade":
                self._last_trade[int(rec["symbol"])] = int(rec["price"])
            elif kind == "order":
                order = Order(
                    id=int(rec["id"]),
                    side=rec["side"],
                    price=int(rec["price"]),
                    quantity=int(rec["quantity"]),
                    account=int(rec["account"]),
                    tif=rec.get("tif", "gtc"),
                    symbol=int(rec["symbol"]),
                    display=int(rec["display"]),
                    visible=int(rec["visible"]),
                    expire_at=int(rec["expire_at"]),
                )
                self.book(order.symbol).restore_resting(order)
                if order.expire_at:
                    self._expire_at[order.id] = order.expire_at
                reserve = int(rec["cash_reserve_qty"])
                if self._enforce_cash and reserve:
                    self._reserve_cash(order.id, order.account, order.price, reserve)
            elif kind == "stop":
                stop = _Stop(
                    id=int(rec["id"]),
                    side=rec["side"],
                    stop_price=int(rec["stop_price"]),
                    quantity=int(rec["quantity"]),
                    account=int(rec["account"]),
                    limit_price=rec.get("limit_price"),
                    tif=rec.get("tif", "gtc"),
                    symbol=int(rec["symbol"]),
                    expire_at=int(rec.get("expire_at", 0)),
                )
                self._stops.setdefault(stop.symbol, []).append(_PendingStop(event=stop))
                self._stop_index[stop.id] = stop.symbol
                if stop.expire_at:
                    self._expire_at[stop.id] = stop.expire_at
            else:
                raise ValueError(f"unknown snapshot type: {kind}")

    def realized_pnl(self, account: int, symbol: int = 0) -> int:
        return self._realized.get((account, symbol), 0)

    def unrealized_pnl(
        self, account: int, symbol: int = 0, mark: Literal["last_trade", "mid"] = "last_trade"
    ) -> Optional[int]:
        price = self.mark_price(mark, symbol)
        if price is None:
            return None
        qty = self.position(account, symbol)
        if qty == 0:
            return 0
        return (price - self._avg_ticks.get((account, symbol), 0)) * qty

    def account_report(
        self, account: int, mark: Literal["last_trade", "mid"] = "last_trade"
    ) -> dict:
        positions: list[dict] = []
        realized_total = 0
        unrealized_total = 0
        inventory_mark = 0
        missing_mark = False
        symbols = {
            symbol
            for (acc, symbol), qty in self._positions.items()
            if acc == account and (qty != 0 or self._realized.get((acc, symbol), 0) != 0)
        }
        symbols |= {
            symbol
            for (acc, symbol), realized in self._realized.items()
            if acc == account and realized != 0
        }
        for symbol in sorted(symbols):
            qty = self.position(account, symbol)
            realized = self.realized_pnl(account, symbol)
            if qty == 0 and realized == 0:
                continue
            row: dict = {
                "symbol": symbol,
                "quantity": qty,
                "avg_ticks": self._avg_ticks.get((account, symbol), 0),
                "realized_pnl": realized,
                "mark_ticks": None,
                "unrealized_pnl": None,
            }
            realized_total += realized
            if qty != 0:
                price = self.mark_price(mark, symbol)
                if price is None:
                    missing_mark = True
                else:
                    row["mark_ticks"] = price
                    row["unrealized_pnl"] = (price - row["avg_ticks"]) * qty
                    unrealized_total += row["unrealized_pnl"]
                    inventory_mark += qty * price
            positions.append(row)

        cash = self.cash(account)
        reserved = self.reserved_cash(account)
        return {
            "account": account,
            "cash": cash,
            "reserved": reserved,
            "available": cash - reserved,
            "fees_paid": self.fees_paid(account),
            "positions": positions,
            "realized_pnl": realized_total,
            "unrealized_pnl": None if missing_mark else unrealized_total,
            "inventory_mark": inventory_mark,
            "equity": cash + inventory_mark,
        }

    def fees_paid(self, account: int) -> int:
        return self._fees_paid.get(account, 0)

    def last_trade_price(self, symbol: int = 0) -> Optional[int]:
        return self._last_trade.get(symbol)

    def mid_price(self, symbol: int = 0) -> Optional[int]:
        book = self.book(symbol)
        bid = book.best_bid()
        ask = book.best_ask()
        if bid is None or ask is None:
            return None
        return (bid + ask) // 2

    def mark_price(
        self, source: Literal["last_trade", "mid"] = "last_trade", symbol: int = 0
    ) -> Optional[int]:
        if source == "mid":
            return self.mid_price(symbol)
        return self.last_trade_price(symbol)

    def pending_stop_count(self, symbol: int = 0) -> int:
        return len(self._stops.get(symbol, []))

    def snapshot(self, max_levels: int, symbol: int = 0):
        return self.book(symbol).snapshot(max_levels)

    def add_limit(self, event: LimitEvent) -> list[Trade]:
        expire_at = event.expire_at
        if event.tif == "gtd":
            if expire_at == 0 or expire_at <= self._now:
                return []
        else:
            expire_at = 0
        if event.reduce_only and not self._allows_reduce_only(
            event.account, event.side, event.quantity, event.symbol
        ):
            return []
        working_sell = self._working_sell(event.account, event.symbol)
        if self._enforce_cash and event.side == "buy":
            if self.available_cash(event.account) < event.price * event.quantity:
                return []
        if self._enforce_cash and event.side == "sell":
            uncovered = self._short_qty(
                event.account, event.symbol, event.quantity, working_sell
            )
            if uncovered and self.available_cash(event.account) < event.price * uncovered:
                return []
        order = Order(
            id=event.id,
            side=event.side,
            price=event.price,
            quantity=event.quantity,
            account=event.account,
            tif=event.tif,
            symbol=event.symbol,
            post_only=event.post_only,
            reduce_only=event.reduce_only,
            display=event.display,
            expire_at=expire_at,
        )
        original = event.quantity
        trades = self.book(event.symbol).add_limit(order)
        self._release_cash_rests_from_trades(trades)
        filled = sum(trade.quantity for trade in trades)
        rested = original - filled
        if (
            self._enforce_cash
            and event.tif in ("gtc", "gtd")
            and rested > 0
            and self.book(event.symbol).is_live(event.id)
        ):
            if event.side == "buy":
                self._reserve_cash(event.id, event.account, event.price, rested)
            else:
                cover = self._free_long(event.account, event.symbol, working_sell)
                covered_fill = min(filled, cover)
                cover_left = cover - covered_fill
                short_rest = max(0, rested - cover_left)
                if short_rest:
                    self._reserve_cash(event.id, event.account, event.price, short_rest)
        if event.tif in ("gtc", "gtd") and rested > 0 and self.book(event.symbol).is_live(
            event.id
        ):
            if expire_at:
                self._expire_at[event.id] = expire_at
        self._note_trades(event.symbol, trades, event.side)
        trades.extend(self._drain_stops(event.symbol))
        return trades

    def add_market(self, event: MarketEvent) -> list[Trade]:
        if event.reduce_only and not self._allows_reduce_only(
            event.account, event.side, event.quantity, event.symbol
        ):
            return []
        if self._enforce_cash and event.side == "buy":
            need = self.book(event.symbol).estimate_buy_notional(
                event.quantity, is_market=True
            )
            if self.available_cash(event.account) < need:
                return []
        if self._enforce_cash and event.side == "sell":
            working_sell = self._working_sell(event.account, event.symbol)
            uncovered = self._short_qty(
                event.account, event.symbol, event.quantity, working_sell
            )
            if uncovered:
                need = self.book(event.symbol).estimate_sell_notional(
                    uncovered, is_market=True
                )
                if self.available_cash(event.account) < need:
                    return []
        order = Order(
            id=event.id,
            side=event.side,
            price=0,
            quantity=event.quantity,
            account=event.account,
            symbol=event.symbol,
            reduce_only=event.reduce_only,
        )
        trades = self.book(event.symbol).add_market(order)
        self._release_cash_rests_from_trades(trades)
        self._note_trades(event.symbol, trades, event.side)
        trades.extend(self._drain_stops(event.symbol))
        return trades

    def add_stop(self, event: StopEvent) -> list[Trade]:
        if event.expire_at != 0 and event.expire_at <= self._now:
            return []
        if self._is_triggered(event):
            return self._fire_stop(event)
        self._stops.setdefault(event.symbol, []).append(_PendingStop(event=event))
        self._stop_index[event.id] = event.symbol
        if event.expire_at:
            self._expire_at[event.id] = event.expire_at
        return []

    def cancel(self, order_id: int) -> bool:
        symbol = self._stop_index.pop(order_id, None)
        if symbol is not None:
            pending = self._stops.get(symbol, [])
            self._stops[symbol] = [item for item in pending if item.event.id != order_id]
            self._expire_at.pop(order_id, None)
            return True

        self._release_cash_rest(order_id)
        self._expire_at.pop(order_id, None)
        for book in self._books.values():
            if book.cancel(order_id):
                return True
        return False

    def replace(self, order_id: int, price: int, quantity: int) -> Optional[list[Trade]]:
        if order_id in self._stop_index:
            return None
        for symbol, book in self._books.items():
            original = next((o for o in book.live_orders() if o.id == order_id), None)
            expire_at = self._expire_at.get(order_id, 0)
            trades = book.replace(order_id, price, quantity)
            if trades is None:
                continue
            self._release_cash_rest(order_id)
            self._expire_at.pop(order_id, None)
            self._release_cash_rests_from_trades(trades)
            side = original.side if original is not None else "buy"
            if (
                self._enforce_cash
                and original is not None
                and quantity > 0
                and book.is_live(order_id)
            ):
                filled = sum(trade.quantity for trade in trades)
                rested = quantity - filled
                if rested > 0:
                    if original.side == "buy":
                        self._reserve_cash(order_id, original.account, price, rested)
                    else:
                        working_sell = self._working_sell(original.account, symbol)
                        # After cancel, working no longer includes this order.
                        cover = self._free_long(original.account, symbol, working_sell)
                        covered_fill = min(filled, cover)
                        cover_left = cover - covered_fill
                        short_rest = max(0, rested - cover_left)
                        if short_rest:
                            self._reserve_cash(
                                order_id, original.account, price, short_rest
                            )
            if quantity > 0 and book.is_live(order_id) and expire_at:
                self._expire_at[order_id] = expire_at
            self._note_trades(symbol, trades, side)
            trades.extend(self._drain_stops(symbol))
            return trades
        return None

    def mass_cancel(
        self,
        account: Optional[int] = None,
        symbol: Optional[int] = None,
        side: Optional[Literal["buy", "sell"]] = None,
    ) -> int:
        ids: list[int] = []
        for sym, pending in self._stops.items():
            if symbol is not None and sym != symbol:
                continue
            for item in pending:
                event = item.event
                if account is not None and event.account != account:
                    continue
                if side is not None and event.side != side:
                    continue
                ids.append(event.id)
        for sym, book in self._books.items():
            if symbol is not None and sym != symbol:
                continue
            for order in book.live_orders():
                if account is not None and order.account != account:
                    continue
                if side is not None and order.side != side:
                    continue
                ids.append(order.id)

        cancelled = 0
        for order_id in dict.fromkeys(ids):
            if self.cancel(order_id):
                cancelled += 1
        return cancelled

    def apply(self, event: Event) -> list[Trade]:
        if isinstance(event, LimitEvent):
            return self.add_limit(event)
        if isinstance(event, MarketEvent):
            return self.add_market(event)
        if isinstance(event, StopEvent):
            return self.add_stop(event)
        if isinstance(event, ReplaceEvent):
            trades = self.replace(event.id, event.price, event.quantity)
            return trades if trades is not None else []
        if isinstance(event, MassCancelEvent):
            self.mass_cancel(account=event.account, symbol=event.symbol, side=event.side)
            return []
        if isinstance(event, RejectEvent):
            return []
        if isinstance(event, TimeEvent):
            self.advance_time(event.time)
            return []
        self.cancel(event.id)
        return []

    def _allows_reduce_only(
        self, account: int, side: Literal["buy", "sell"], quantity: int, symbol: int
    ) -> bool:
        pos = self.position(account, symbol)
        if side == "buy":
            return pos < 0 and quantity <= -pos
        return pos > 0 and quantity <= pos

    def _working_sell(self, account: int, symbol: int) -> int:
        total = 0
        for order in self.book(symbol).live_orders():
            if order.account == account and order.side == "sell":
                total += order.quantity
        for item in self._stops.get(symbol, []):
            if item.event.account == account and item.event.side == "sell":
                total += item.event.quantity
        return total

    def _free_long(self, account: int, symbol: int, working_sell: int) -> int:
        pos = self.position(account, symbol)
        long_qty = pos if pos > 0 else 0
        return max(0, long_qty - working_sell)

    def _short_qty(
        self, account: int, symbol: int, quantity: int, working_sell: int
    ) -> int:
        cover = self._free_long(account, symbol, working_sell)
        return max(0, quantity - cover)

    def _reserve_cash(self, order_id: int, account: int, price: int, quantity: int) -> None:
        amount = price * quantity
        self._reserved[account] = self.reserved_cash(account) + amount
        self._cash_rests[order_id] = (account, price, quantity)

    def _release_cash_rest(self, order_id: int, quantity: Optional[int] = None) -> None:
        rest = self._cash_rests.get(order_id)
        if rest is None:
            return
        account, price, remaining = rest
        release_qty = remaining if quantity is None else min(quantity, remaining)
        amount = price * release_qty
        held = self.reserved_cash(account) - amount
        if held <= 0:
            self._reserved.pop(account, None)
        else:
            self._reserved[account] = held
        left = remaining - release_qty
        if left <= 0:
            self._cash_rests.pop(order_id, None)
        else:
            self._cash_rests[order_id] = (account, price, left)

    def _release_cash_rests_from_trades(self, trades: list[Trade]) -> None:
        for trade in trades:
            self._release_cash_rest(trade.maker_id, trade.quantity)

    def _note_trades(
        self, symbol: int, trades: list[Trade], taker_side: Literal["buy", "sell"]
    ) -> None:
        stamped: list[Trade] = []
        for trade in trades:
            stamped.append(
                Trade(
                    maker_id=trade.maker_id,
                    taker_id=trade.taker_id,
                    maker_account=trade.maker_account,
                    taker_account=trade.taker_account,
                    price=trade.price,
                    quantity=trade.quantity,
                    id=self._next_trade_id,
                )
            )
            self._next_trade_id += 1
            delta = trade.quantity if taker_side == "buy" else -trade.quantity
            self._apply_fill(trade.taker_account, symbol, delta, trade.price)
            self._apply_fill(trade.maker_account, symbol, -delta, trade.price)
            notional = trade.price * trade.quantity
            maker_fee = (notional * self._maker_bps) // 10_000
            taker_fee = (notional * self._taker_bps) // 10_000
            self._fees_paid[trade.maker_account] = (
                self._fees_paid.get(trade.maker_account, 0) + maker_fee
            )
            self._fees_paid[trade.taker_account] = (
                self._fees_paid.get(trade.taker_account, 0) + taker_fee
            )
            if taker_side == "buy":
                self._cash[trade.taker_account] = (
                    self.cash(trade.taker_account) - (notional + taker_fee)
                )
                self._cash[trade.maker_account] = (
                    self.cash(trade.maker_account) + (notional - maker_fee)
                )
            else:
                self._cash[trade.taker_account] = (
                    self.cash(trade.taker_account) + (notional - taker_fee)
                )
                self._cash[trade.maker_account] = (
                    self.cash(trade.maker_account) - (notional + maker_fee)
                )
        trades[:] = stamped
        if stamped:
            self._last_trade[symbol] = stamped[-1].price

    def _apply_fill(self, account: int, symbol: int, delta: int, price: int) -> None:
        key = (account, symbol)
        qty = self._positions.get(key, 0)
        avg = self._avg_ticks.get(key, 0)
        realized = self._realized.get(key, 0)

        if qty == 0 or (qty > 0) == (delta > 0):
            abs_old = abs(qty)
            abs_add = abs(delta)
            self._avg_ticks[key] = (abs_old * avg + abs_add * price) // (abs_old + abs_add)
            self._positions[key] = qty + delta
            return

        close_qty = min(abs(delta), abs(qty))
        if qty > 0:
            realized += (price - avg) * close_qty
        else:
            realized += (avg - price) * close_qty

        previous = qty
        qty += delta
        self._realized[key] = realized
        self._positions[key] = qty
        if qty == 0:
            self._avg_ticks[key] = 0
        elif (previous > 0) != (qty > 0):
            self._avg_ticks[key] = price

    def _is_triggered(self, event: StopEvent) -> bool:
        last = self._last_trade.get(event.symbol)
        if last is None:
            return False
        if event.side == "buy":
            return last >= event.stop_price
        return last <= event.stop_price

    def _fire_stop(self, event: StopEvent) -> list[Trade]:
        if event.limit_price is None:
            return self.add_market(
                MarketEvent(
                    id=event.id,
                    side=event.side,
                    quantity=event.quantity,
                    account=event.account,
                    symbol=event.symbol,
                )
            )
        return self.add_limit(
            LimitEvent(
                id=event.id,
                side=event.side,
                price=event.limit_price,
                quantity=event.quantity,
                account=event.account,
                tif=event.tif,
                symbol=event.symbol,
                expire_at=event.expire_at if event.tif == "gtd" else 0,
            )
        )

    def _drain_stops(self, symbol: int) -> list[Trade]:
        trades: list[Trade] = []
        progressed = True
        while progressed:
            progressed = False
            pending = self._stops.get(symbol, [])
            for index, item in enumerate(pending):
                if not self._is_triggered(item.event):
                    continue
                stop = pending.pop(index).event
                self._stops[symbol] = pending
                self._stop_index.pop(stop.id, None)
                self._expire_at.pop(stop.id, None)
                trades.extend(self._fire_stop(stop))
                progressed = True
                break
        return trades
