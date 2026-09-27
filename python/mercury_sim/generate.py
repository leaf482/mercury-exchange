from __future__ import annotations

import argparse
import random

from mercury_sim.events import (
    CancelEvent,
    Event,
    LimitEvent,
    MarketEvent,
    MassCancelEvent,
    RejectEvent,
    ReplaceEvent,
    StopEvent,
    TimeEvent,
    write_jsonl,
)
from mercury_sim.sim import SimConfig, simulate_market


def generate_events(count: int, seed: int = 1, start_price: int = 10_000) -> list[Event]:
    rng = random.Random(seed)
    events: list[Event] = []
    next_id = 1
    mid = start_price
    live: list[int] = []

    for _ in range(count):
        roll = rng.random()
        if live and roll < 0.15:
            order_id = live.pop(rng.randrange(len(live)))
            events.append(CancelEvent(id=order_id))
            continue

        side = "buy" if rng.random() < 0.5 else "sell"
        if roll < 0.25:
            events.append(
                MarketEvent(
                    id=next_id,
                    side=side,
                    quantity=rng.randint(1, 5),
                    account=rng.randint(1, 3),
                )
            )
            next_id += 1
            continue

        mid += rng.choice([-1, 0, 0, 1])
        price = mid - rng.randint(0, 5) if side == "buy" else mid + rng.randint(0, 5)
        qty = rng.randint(1, 10)
        events.append(
            LimitEvent(
                id=next_id,
                side=side,
                price=price,
                quantity=qty,
                account=rng.randint(1, 3),
            )
        )
        live.append(next_id)
        next_id += 1

    return events


def generate_stress_events(
    count: int,
    seed: int = 1,
    start_price: int = 1_000,
    symbols: int = 4,
    accounts: tuple[int, ...] = (1, 2, 3),
) -> list[Event]:
    """Seeded mix of limits, markets, stops, cancels, and clock ticks.

    Prices stay positive. GTD deadlines are strictly after the generator clock.
    Reject lines are audit-only and must not change either engine.
    """
    rng = random.Random(seed)
    events: list[Event] = []
    next_id = 1
    now = 0
    mids = {symbol: start_price for symbol in range(symbols)}
    issued: list[int] = []

    def take_id() -> int:
        nonlocal next_id
        order_id = next_id
        next_id += 1
        issued.append(order_id)
        return order_id

    def choose_symbol() -> int:
        return rng.randrange(symbols)

    def choose_side() -> str:
        return "buy" if rng.random() < 0.5 else "sell"

    def next_price(symbol: int, side: str) -> int:
        mids[symbol] = max(2, mids[symbol] + rng.choice([-1, 0, 0, 1]))
        offset = rng.randint(0, 4)
        price = mids[symbol] - offset if side == "buy" else mids[symbol] + offset
        return max(1, price)

    for _ in range(count):
        roll = rng.random()
        if issued and roll < 0.12:
            events.append(CancelEvent(id=issued[rng.randrange(len(issued))]))
            continue
        if issued and roll < 0.20:
            symbol = choose_symbol()
            side = choose_side()
            events.append(
                ReplaceEvent(
                    id=issued[rng.randrange(len(issued))],
                    price=next_price(symbol, side),
                    quantity=rng.randint(1, 8),
                )
            )
            continue
        if roll < 0.26:
            account = rng.choice(accounts) if rng.random() < 0.7 else None
            symbol = choose_symbol() if rng.random() < 0.7 else None
            side = choose_side() if rng.random() < 0.5 else None
            events.append(MassCancelEvent(account=account, symbol=symbol, side=side))
            continue
        if roll < 0.34:
            now += rng.randint(1, 4)
            events.append(TimeEvent(time=now))
            continue
        if roll < 0.38:
            side = choose_side()
            events.append(
                RejectEvent(
                    decision="post_only",
                    order_type="limit",
                    id=take_id(),
                    side=side,
                    quantity=1,
                    price=next_price(0, side),
                    account=rng.choice(accounts),
                )
            )
            continue

        side = choose_side()
        symbol = choose_symbol()
        account = rng.choice(accounts)
        quantity = rng.randint(1, 8)
        if roll < 0.50:
            events.append(
                MarketEvent(
                    id=take_id(),
                    side=side,
                    quantity=quantity,
                    account=account,
                    symbol=symbol,
                    reduce_only=rng.random() < 0.08,
                )
            )
            continue
        if roll < 0.62:
            stop_price = next_price(symbol, side)
            limit_price = stop_price if rng.random() < 0.5 else None
            expire_at = 0
            tif = "gtc"
            if rng.random() < 0.25:
                tif = "gtd"
                expire_at = now + rng.randint(1, 12)
            events.append(
                StopEvent(
                    id=take_id(),
                    side=side,
                    stop_price=stop_price,
                    quantity=quantity,
                    account=account,
                    limit_price=limit_price,
                    tif=tif,
                    symbol=symbol,
                    expire_at=expire_at,
                )
            )
            continue

        tif = rng.choice(("gtc", "gtc", "gtc", "gtc", "ioc", "fok", "gtd"))
        expire_at = now + rng.randint(1, 15) if tif == "gtd" else 0
        display = rng.randint(1, quantity) if rng.random() < 0.18 else 0
        events.append(
            LimitEvent(
                id=take_id(),
                side=side,
                price=next_price(symbol, side),
                quantity=quantity,
                account=account,
                tif=tif,
                symbol=symbol,
                post_only=rng.random() < 0.12,
                reduce_only=rng.random() < 0.08,
                display=display,
                expire_at=expire_at,
            )
        )

    return events


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Generate synthetic Mercury event JSONL")
    parser.add_argument("-n", "--count", type=int, default=100)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--start-price", type=int, default=10_000)
    parser.add_argument(
        "--mode",
        choices=("random", "market", "stress"),
        default="random",
        help="random noise, inventory-aware market sim, or mixed feature stress",
    )
    parser.add_argument(
        "--arrival-rate",
        type=float,
        default=0.55,
        help="Bernoulli arrival probability per tick (market mode)",
    )
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args(argv)

    if args.mode == "market":
        events = simulate_market(
            SimConfig(
                n_events=args.count,
                seed=args.seed,
                start_price=args.start_price,
                arrival_rate=args.arrival_rate,
            )
        )
    elif args.mode == "stress":
        events = generate_stress_events(
            args.count, seed=args.seed, start_price=args.start_price
        )
    else:
        events = generate_events(args.count, seed=args.seed, start_price=args.start_price)

    write_jsonl(args.output, events)
    print(f"wrote {len(events)} events to {args.output}")


if __name__ == "__main__":
    main()
