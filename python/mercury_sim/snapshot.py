from __future__ import annotations

import json

from mercury_sim.book import Order
from mercury_sim.engine import Engine
from mercury_sim.events import StopEvent


def _line(payload: dict) -> str:
    return json.dumps(payload, separators=(",", ":"))


def lines_from_engine(engine: Engine) -> list[str]:
    lines = [
        _line(
            {
                "type": "meta",
                "now": engine.now(),
                "next_trade_id": engine.next_trade_id(),
                "enforce_cash": engine.enforce_cash(),
                "maker_bps": engine.maker_bps(),
                "taker_bps": engine.taker_bps(),
                "stp": engine.stp(),
                "max_order_quantity": 0,
                "max_abs_position": 0,
            }
        )
    ]
    for account, amount in sorted(engine.cash_balances()):
        if amount != 0:
            lines.append(_line({"type": "cash", "account": account, "amount": amount}))
    for account, amount in sorted(engine.fees_balances()):
        if amount != 0:
            lines.append(
                _line({"type": "fees_paid", "account": account, "amount": amount})
            )
    for row in engine.position_rows():
        lines.append(
            _line(
                {
                    "type": "position",
                    "account": row["account"],
                    "symbol": row["symbol"],
                    "quantity": row["quantity"],
                    "avg_ticks": row["avg_ticks"],
                    "realized_pnl": row["realized_pnl"],
                }
            )
        )
    for symbol, price in engine.last_trades():
        lines.append(_line({"type": "last_trade", "symbol": symbol, "price": price}))
    for order, reserve in engine.resting_rows():
        lines.append(
            _line(
                {
                    "type": "order",
                    "id": order.id,
                    "side": order.side,
                    "price": order.price,
                    "quantity": order.quantity,
                    "account": order.account,
                    "tif": order.tif,
                    "symbol": order.symbol,
                    "display": order.display,
                    "visible": order.visible,
                    "expire_at": order.expire_at,
                    "cash_reserve_qty": reserve,
                }
            )
        )
    for stop in engine.pending_stops():
        payload = {
            "type": "stop",
            "id": stop.id,
            "side": stop.side,
            "stop_price": stop.stop_price,
            "quantity": stop.quantity,
            "account": stop.account,
        }
        if stop.limit_price is not None:
            payload["limit_price"] = stop.limit_price
        payload["tif"] = stop.tif
        payload["symbol"] = stop.symbol
        payload["expire_at"] = stop.expire_at
        lines.append(_line(payload))
    return lines


def engine_from_lines(lines: list[str]) -> Engine:
    records = [json.loads(line) for line in lines if line.strip()]
    if not records or records[0]["type"] != "meta":
        raise ValueError("snapshot missing meta")
    meta = records[0]
    if meta["max_order_quantity"] or meta["max_abs_position"]:
        raise ValueError("python engine has no configurable risk limits")
    engine = Engine(
        stp="cancel_resting" if meta["stp"] == "cancel_resting" else "off",
        maker_bps=int(meta["maker_bps"]),
        taker_bps=int(meta["taker_bps"]),
        enforce_cash=bool(meta["enforce_cash"]),
    )
    engine.load_restart_records(records)
    return engine
