# Roadmap

Incremental work only. Correctness and measurement before concurrency.

## Progress (portfolio core)

Matching / risk / accounting for a single-threaded exchange sim is complete
for the intended portfolio core. Optional work is outside the matching path
(net/UI) or concurrency after a measured hotspot. See `README.md` for the
recruiter-oriented overview.

| Area | Status |
| --- | --- |
| Matching (limit/market/cancel, TIF+GTD, stop, replace, mass cancel, iceberg) | Done |
| Risk (size/position, STP, post-only, reduce-only, cash+buy/short reserve) | Done |
| Positions / PnL / fees / TradeId / account report | Done |
| Deterministic JSONL + Engine replay + Python parity | Done |
| Per-operation latency (median / p95 / p99) | Done — no shard hotspot |
| Networking / UI / multi-thread shards | Not started (deferred) |

## Done

- Limit / market / cancel matching (price-time priority, partial fills)
- IOC / FOK time-in-force on limit orders (GTC default)
- Stop orders (last-trade trigger → limit or market)
- Multi-instrument books keyed by `Symbol` (positions/risk per symbol)
- OrderId index, positions + PnL, pre-trade risk (incl. resting exposure)
- Deterministic event log + JSONL save/load + Engine replay (incl. stops)
- Book depth snapshot (`book_snapshot`)
- Latency benches (rest / match / cancel / deep book / multi-symbol / mass cancel)
- Python generate / replay / C++ parity compare (tif + stop)
- Python market sim (Bernoulli arrivals, inventory-skewed maker + takers)
- Optional pybind11 `mercury_engine` module (`Engine` / `snapshot`)
- Self-trade prevention (`CancelResting`; account 0 exempt; default off)
- Order replace (cancel-replace; loses time priority; JSONL `replace`)
- Mass cancel by account / symbol / side (JSONL `mass_cancel`)
- Maker/taker fees in bps (tick×qty units; optional rebates)
- Post-only limits (`post_only`; Engine `RiskDecision::PostOnly`)
- Reduce-only limits/markets (no open / increase / flip; `RiskDecision::ReduceOnly`)
- Unrealized PnL marked at last trade or mid (`MarkSource`; default last trade)
- Monotonic `TradeId` stamped by Engine (OrderBook leaves 0)
- Account cash ledger (tick×qty); optional buy cash enforcement + resting reserve
- Reject audit events (JSONL `reject`; replay no-op)
- Iceberg limits (`display` peak; tip-refill requeues and loses time priority)
- GTD limits (`tif=gtd` + `expire_at`); JSONL `time` advances Engine clock and expires
- Pending stops may set `expire_at` and expire on the same clock
- Short margin under `enforce_cash`: uncovered sells need/reserve cash like buys
- Account report (cash / reserved / positions / mark PnL / equity)
- `account_report` CLI + iceberg/account_report latency benches (refreshed)
- Per-operation latency sampler (`mercury_latency`: 10k samples, cycle counter)

## Next (small steps)

1. Optional networking / storage / UI outside the matching core
2. Shard-by-symbol only if future benches show a real hotspot (none yet; see
   `docs/latency.md`)

## Later

- Threading / shard-by-symbol only after single-thread benches justify it
- Networking, storage backends, UI — outside the matching core

## Not planned soon

Lock-free structures, Kafka/Redis, Docker-centric deploys, or rewriting the
engine for throughput before profiling a concrete bottleneck.
