from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from mercury_sim.engine import Engine
from mercury_sim.events import Event, read_jsonl, write_jsonl
from mercury_sim.generate import generate_events, generate_stress_events
from mercury_sim.replay import replay


def find_app(name: str, repo_root: Path | None = None) -> Path | None:
    root = repo_root or Path(__file__).resolve().parents[2]
    candidates = []
    for build in ("build-release", "build"):
        for suffix in (".exe", ""):
            candidates.append(root / build / "apps" / f"{name}{suffix}")
    for path in candidates:
        if path.is_file():
            return path
    return None


def find_jsonl_replay(repo_root: Path | None = None) -> Path | None:
    return find_app("jsonl_replay", repo_root)


def load_trade_dicts(text: str) -> list[dict]:
    trades: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if line:
            trades.append(json.loads(line))
    return trades


def cpp_trades(events_path: Path, replay_bin: Path) -> list[dict]:
    result = subprocess.run(
        [str(replay_bin), str(events_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return load_trade_dicts(result.stdout)


def python_trades(events_path: Path) -> list[dict]:
    trades, _ = replay(read_jsonl(str(events_path)))
    return [trade.to_dict() for trade in trades]


def compare_files(events_path: Path, replay_bin: Path) -> tuple[list[dict], list[dict]]:
    return python_trades(events_path), cpp_trades(events_path, replay_bin)


def touched_symbols(events: list[Event]) -> list[int]:
    symbols = {0}
    for event in events:
        symbol = getattr(event, "symbol", None)
        if isinstance(symbol, int):
            symbols.add(symbol)
    return sorted(symbols)


def touched_accounts(events: list[Event]) -> list[int]:
    accounts: set[int] = set()
    for event in events:
        account = getattr(event, "account", None)
        if isinstance(account, int):
            accounts.add(account)
    return sorted(accounts) or [0]


def _levels(rows) -> list[dict]:
    return [
        {"price": row.price, "quantity": row.quantity, "order_count": row.order_count}
        for row in rows
    ]


def _canon_report(report: dict) -> dict:
    positions = sorted(report["positions"], key=lambda row: row["symbol"])
    return {
        "cash": report["cash"],
        "reserved": report["reserved"],
        "available": report["available"],
        "fees_paid": report["fees_paid"],
        "realized_pnl": report["realized_pnl"],
        "unrealized_pnl": report["unrealized_pnl"],
        "inventory_mark": report["inventory_mark"],
        "equity": report["equity"],
        "positions": positions,
    }


def python_invariants(
    events: list[Event], symbols: list[int], accounts: list[int], depth: int
) -> dict:
    engine = Engine()
    trades = []
    for event in events:
        trades.extend(engine.apply(event))
    books = {}
    for symbol in symbols:
        snap = engine.snapshot(depth, symbol)
        books[symbol] = {"bids": _levels(snap.bids), "asks": _levels(snap.asks)}
    reports = {}
    for account in accounts:
        reports[account] = {
            "last": _canon_report(engine.account_report(account, "last_trade")),
            "mid": _canon_report(engine.account_report(account, "mid")),
        }
    return {
        "trades": [trade.to_dict() for trade in trades],
        "books": books,
        "reports": reports,
    }


def _run_json(bin_path: Path, args: list[str]) -> dict:
    result = subprocess.run(
        [str(bin_path), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    line = result.stdout.strip().splitlines()[-1]
    return json.loads(line)


def cpp_invariants(
    events_path: Path,
    replay_bin: Path,
    book_bin: Path,
    report_bin: Path,
    symbols: list[int],
    accounts: list[int],
    depth: int,
) -> dict:
    books = {}
    for symbol in symbols:
        raw = _run_json(book_bin, [str(events_path), str(depth), str(symbol)])
        books[symbol] = raw
    reports = {}
    for account in accounts:
        reports[account] = {
            "last": _canon_report(
                _run_json(report_bin, [str(events_path), str(account), "last"])
            ),
            "mid": _canon_report(
                _run_json(report_bin, [str(events_path), str(account), "mid"])
            ),
        }
    return {
        "trades": cpp_trades(events_path, replay_bin),
        "books": books,
        "reports": reports,
    }


def invariant_mismatch(python_state: dict, cpp_state: dict) -> str | None:
    py_trades = python_state["trades"]
    cxx_trades = cpp_state["trades"]
    if py_trades != cxx_trades:
        limit = min(len(py_trades), len(cxx_trades))
        for index in range(limit):
            if py_trades[index] != cxx_trades[index]:
                return (
                    f"trade[{index}]\npython={py_trades[index]}\ncpp={cxx_trades[index]}"
                )
        return f"trades python={len(py_trades)} cpp={len(cxx_trades)}"
    for symbol, book in python_state["books"].items():
        other = cpp_state["books"].get(symbol)
        if book != other:
            return f"book symbol={symbol}\npython={book}\ncpp={other}"
    for account, marks in python_state["reports"].items():
        other = cpp_state["reports"].get(account)
        if marks != other:
            return f"account={account}\npython={marks}\ncpp={other}"
    return None


def compare_invariants(
    events_path: Path,
    replay_bin: Path,
    book_bin: Path,
    report_bin: Path,
    depth: int = 32,
) -> dict:
    events = read_jsonl(str(events_path))
    symbols = touched_symbols(events)
    accounts = touched_accounts(events)
    python_state = python_invariants(events, symbols, accounts, depth)
    cpp_state = cpp_invariants(
        events_path, replay_bin, book_bin, report_bin, symbols, accounts, depth
    )
    mismatch = invariant_mismatch(python_state, cpp_state)
    if mismatch is not None:
        raise SystemExit(f"mismatch: {mismatch}")
    return {
        "trades": len(python_state["trades"]),
        "books": len(symbols),
        "accounts": len(accounts),
    }


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="Compare Python and C++ JSONL replays")
    parser.add_argument("events", nargs="?", help="events.jsonl (generated if omitted)")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--stress", action="store_true", help="seeded mixed-feature JSONL")
    parser.add_argument("--replay-bin", type=Path, default=None)
    parser.add_argument("--book-bin", type=Path, default=None)
    parser.add_argument("--report-bin", type=Path, default=None)
    args = parser.parse_args(argv)

    replay_bin = args.replay_bin or find_jsonl_replay()
    book_bin = args.book_bin or find_app("book_snapshot")
    report_bin = args.report_bin or find_app("account_report")
    if replay_bin is None or book_bin is None or report_bin is None:
        raise SystemExit("jsonl_replay, book_snapshot, and account_report must be built")

    if args.events:
        events_path = Path(args.events)
    else:
        events_path = Path("events.jsonl")
        events = (
            generate_stress_events(args.count, seed=args.seed)
            if args.stress
            else generate_events(args.count, seed=args.seed)
        )
        write_jsonl(str(events_path), events)

    summary = compare_invariants(events_path, replay_bin, book_bin, report_bin)
    print(
        f"ok: {summary['trades']} matching trades, "
        f"{summary['books']} books, {summary['accounts']} accounts"
    )


if __name__ == "__main__":
    main()
