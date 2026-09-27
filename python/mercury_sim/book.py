from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Literal, Optional


@dataclass
class Order:
    id: int
    side: Literal["buy", "sell"]
    price: int
    quantity: int
    account: int = 0
    tif: Literal["gtc", "ioc", "fok", "gtd"] = "gtc"
    symbol: int = 0
    post_only: bool = False
    reduce_only: bool = False
    display: int = 0  # peak; 0 = fully visible
    visible: int = 0  # current tip (armed on rest)
    expire_at: int = 0


def arm_iceberg(order: Order) -> None:
    if order.display <= 0:
        order.visible = 0
    else:
        order.visible = min(order.quantity, order.display)


def visible_quantity(order: Order) -> int:
    if order.display <= 0:
        return order.quantity
    if order.visible <= 0:
        return min(order.quantity, order.display)
    return min(order.quantity, order.visible)


@dataclass(frozen=True)
class Trade:
    maker_id: int
    taker_id: int
    maker_account: int
    taker_account: int
    price: int
    quantity: int
    id: int = 0

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "maker_id": self.maker_id,
            "taker_id": self.taker_id,
            "maker_account": self.maker_account,
            "taker_account": self.taker_account,
            "price": self.price,
            "quantity": self.quantity,
        }


@dataclass(frozen=True)
class BookLevel:
    price: int
    quantity: int
    order_count: int


@dataclass(frozen=True)
class BookSnapshot:
    bids: tuple[BookLevel, ...]  # best bid first
    asks: tuple[BookLevel, ...]  # best ask first

    def best_bid(self) -> Optional[int]:
        return self.bids[0].price if self.bids else None

    def best_ask(self) -> Optional[int]:
        return self.asks[0].price if self.asks else None

    def spread_ticks(self) -> Optional[int]:
        bid = self.best_bid()
        ask = self.best_ask()
        if bid is None or ask is None:
            return None
        return ask - bid


class OrderBook:
    def __init__(self, stp: Literal["off", "cancel_resting"] = "off") -> None:
        self._bids: dict[int, deque[Order]] = {}
        self._asks: dict[int, deque[Order]] = {}
        self._index: dict[int, tuple[Literal["buy", "sell"], int]] = {}
        self._stp = stp
        self._stp_cancels: list[int] = []

    def take_stp_cancels(self) -> list[int]:
        cancels = self._stp_cancels
        self._stp_cancels = []
        return cancels

    def add_limit(self, order: Order) -> list[Trade]:
        self._stp_cancels = []
        if order.post_only and self._would_take(order):
            return []
        if order.tif == "fok" and not self._can_fully_fill(order):
            return []

        trades = self._match(order, is_market=False)
        if order.tif in ("gtc", "gtd") and order.quantity > 0:
            self._rest(order)
        return trades

    def add_market(self, order: Order) -> list[Trade]:
        self._stp_cancels = []
        return self._match(order, is_market=True)

    def cancel(self, order_id: int) -> bool:
        loc = self._index.get(order_id)
        if loc is None:
            return False
        side, price = loc
        levels = self._bids if side == "buy" else self._asks
        queue = levels[price]
        for i, order in enumerate(queue):
            if order.id == order_id:
                del queue[i]
                break
        if not queue:
            del levels[price]
        del self._index[order_id]
        return True

    def replace(self, order_id: int, price: int, quantity: int) -> Optional[list[Trade]]:
        """Cancel-replace resting order (loses time priority). qty 0 cancels only."""
        loc = self._index.get(order_id)
        if loc is None:
            return None
        side, old_price = loc
        levels = self._bids if side == "buy" else self._asks
        original = next(order for order in levels[old_price] if order.id == order_id)
        restored = Order(
            id=original.id,
            side=original.side,
            price=price,
            quantity=quantity,
            account=original.account,
            tif="gtd" if original.expire_at else "gtc",
            symbol=original.symbol,
            post_only=original.post_only,
            reduce_only=original.reduce_only,
            display=original.display,
            expire_at=original.expire_at,
        )
        self.cancel(order_id)
        if quantity == 0:
            return []
        return self.add_limit(restored)

    def is_live(self, order_id: int) -> bool:
        return order_id in self._index

    def restore_resting(self, order: Order) -> None:
        """Put an order back on the book without matching or re-arming the tip."""
        levels = self._bids if order.side == "buy" else self._asks
        levels.setdefault(order.price, deque()).append(order)
        self._index[order.id] = (order.side, order.price)

    def resting_orders(self) -> list[Order]:
        orders: list[Order] = []
        for price in sorted(self._bids, reverse=True):
            orders.extend(self._bids[price])
        for price in sorted(self._asks):
            orders.extend(self._asks[price])
        return orders

    def live_orders(self) -> list[Order]:
        orders: list[Order] = []
        for order_id, (side, price) in self._index.items():
            levels = self._bids if side == "buy" else self._asks
            for order in levels[price]:
                if order.id == order_id:
                    orders.append(order)
                    break
        return orders

    def best_bid(self) -> Optional[int]:
        return max(self._bids) if self._bids else None

    def best_ask(self) -> Optional[int]:
        return min(self._asks) if self._asks else None

    def spread(self) -> Optional[int]:
        bid = self.best_bid()
        ask = self.best_ask()
        if bid is None or ask is None:
            return None
        return ask - bid

    def snapshot(self, max_levels: int) -> BookSnapshot:
        bids = tuple(
            BookLevel(
                price=price,
                quantity=sum(visible_quantity(order) for order in self._bids[price]),
                order_count=len(self._bids[price]),
            )
            for price in sorted(self._bids, reverse=True)[:max_levels]
        )
        asks = tuple(
            BookLevel(
                price=price,
                quantity=sum(visible_quantity(order) for order in self._asks[price]),
                order_count=len(self._asks[price]),
            )
            for price in sorted(self._asks)[:max_levels]
        )
        return BookSnapshot(bids=bids, asks=asks)

    def estimate_buy_notional(self, quantity: int, is_market: bool, limit: int = 0) -> int:
        need = 0
        remaining = quantity
        for price in sorted(self._asks):
            if remaining <= 0:
                break
            if not is_market and price > limit:
                break
            level_qty = sum(order.quantity for order in self._asks[price])
            take = min(remaining, level_qty)
            need += price * take
            remaining -= take
        return need

    def estimate_sell_notional(self, quantity: int, is_market: bool, limit: int = 0) -> int:
        need = 0
        remaining = quantity
        for price in sorted(self._bids, reverse=True):
            if remaining <= 0:
                break
            if not is_market and price < limit:
                break
            level_qty = sum(order.quantity for order in self._bids[price])
            take = min(remaining, level_qty)
            need += price * take
            remaining -= take
        return need

    def _would_take(self, order: Order) -> bool:
        if order.side == "buy":
            ask = self.best_ask()
            return ask is not None and order.price >= ask
        bid = self.best_bid()
        return bid is not None and order.price <= bid

    def _can_fully_fill(self, order: Order) -> bool:
        available = 0
        if order.side == "buy":
            for price in sorted(self._asks):
                if price > order.price:
                    break
                available += self._matchable_qty(self._asks[price], order.account)
                if available >= order.quantity:
                    return True
        else:
            for price in sorted(self._bids, reverse=True):
                if price < order.price:
                    break
                available += self._matchable_qty(self._bids[price], order.account)
                if available >= order.quantity:
                    return True
        return False

    def _matchable_qty(self, queue: deque[Order], taker_account: int) -> int:
        total = 0
        for order in queue:
            if self._is_self_trade(order.account, taker_account):
                continue
            total += order.quantity
        return total

    def _is_self_trade(self, maker_account: int, taker_account: int) -> bool:
        return (
            self._stp == "cancel_resting"
            and maker_account == taker_account
            and maker_account != 0
        )

    def _rest(self, order: Order) -> None:
        arm_iceberg(order)
        levels = self._bids if order.side == "buy" else self._asks
        levels.setdefault(order.price, deque()).append(order)
        self._index[order.id] = (order.side, order.price)

    def _match(self, taker: Order, is_market: bool) -> list[Trade]:
        trades: list[Trade] = []
        if taker.side == "buy":
            while taker.quantity > 0 and self._asks:
                best = min(self._asks)
                if not is_market and best > taker.price:
                    break
                self._fill_level(self._asks, best, taker, trades)
        else:
            while taker.quantity > 0 and self._bids:
                best = max(self._bids)
                if not is_market and best < taker.price:
                    break
                self._fill_level(self._bids, best, taker, trades)
        return trades

    def _fill_level(
        self,
        levels: dict[int, deque[Order]],
        price: int,
        taker: Order,
        trades: list[Trade],
    ) -> None:
        queue = levels[price]
        while taker.quantity > 0 and queue:
            maker = queue[0]
            if self._is_self_trade(maker.account, taker.account):
                self._stp_cancels.append(maker.id)
                queue.popleft()
                del self._index[maker.id]
                if not queue:
                    del levels[price]
                continue

            if maker.display > 0 and maker.visible <= 0:
                arm_iceberg(maker)
            tip = maker.quantity if maker.display <= 0 else maker.visible
            fill = min(taker.quantity, tip)
            trades.append(
                Trade(
                    maker_id=maker.id,
                    taker_id=taker.id,
                    maker_account=maker.account,
                    taker_account=taker.account,
                    price=maker.price,
                    quantity=fill,
                )
            )
            taker.quantity -= fill
            maker.quantity -= fill
            if maker.display > 0:
                maker.visible -= fill
            if maker.quantity == 0:
                queue.popleft()
                del self._index[maker.id]
            elif maker.display > 0 and maker.visible == 0:
                arm_iceberg(maker)
                queue.popleft()
                queue.append(maker)
        if price in levels and not levels[price]:
            del levels[price]
