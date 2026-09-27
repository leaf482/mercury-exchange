# Mercury Exchange

Mercury Exchange is a deterministic **C++23** exchange simulator built to study
matching-engine correctness and latency before introducing concurrency. It
implements price-time priority matching, multi-instrument order books,
risk/accounting, advanced order semantics, deterministic JSONL replay, and an
independent Python reference engine for parity testing.

The matching core is intentionally **single-threaded**. A measured match against
one of about **32** populated symbols is about **250 ns median** (p99 about
**490 ns**), so symbol sharding is deferred until measurement shows a real
bottleneck.

This is a **personal / learning / portfolio project**. It is not connected to
live markets or real money.

**C++23 · CMake · GoogleTest · Google Benchmark · Python · optional pybind11**

Further reading: [`docs/architecture.md`](docs/architecture.md) ·
[`docs/latency.md`](docs/latency.md) · [`docs/roadmap.md`](docs/roadmap.md)

## Architecture

```text
Event / CLI / Python
        |
        v
      Engine
        |
        +-- Risk checks
        +-- Symbol -> OrderBook
        |              +-- PriceLevel queues
        |              +-- price-time (FIFO) priority
        +-- Positions / PnL
        +-- Balances / Fees
        +-- EventLog / JSONL replay
```

Details and semantics: [`docs/architecture.md`](docs/architecture.md).

## Key capabilities

### Matching

- Limit / market / cancel; partial fills; trade price = maker (resting) price
- Time-in-force: GTC, IOC, FOK, GTD (discrete Engine clock + `expire_at`)
- Stop orders (last-trade trigger → limit or market); pending stops may expire
- Replace (cancel-replace; loses time priority) and mass cancel
- Self-trade prevention (`CancelResting`), post-only, reduce-only
- Iceberg tip-refill (display peak; refill requeues and loses time priority)
- Multi-instrument books keyed by `Symbol`

### Risk & accounting

- Max order size; max absolute position including resting exposure
- Maker/taker fees (bps); cumulative `fees_paid`
- Cash ledger with optional enforcement, buy reserves, and short-margin checks
- Realized / unrealized PnL (last-trade or mid mark)
- Account equity report (cash, reserved, positions, mark inventory)

### Determinism & tooling

- Integer ticks / lots in the matching hot path
- JSONL save / load / Engine replay
- Python twin simulator and C++/Python trade parity compare
- Optional pybind11 `mercury_engine` bindings
- CLIs: `jsonl_replay`, `book_snapshot`, `account_report`

## Correctness

Approximate automated coverage (counts drift as the suite grows):

- ~152 C++ GoogleTest cases
- ~48 Python `unittest` cases
- C++/Python trade parity via `mercury_sim.compare`

The Python engine is an independent reference implementation of matching and
accounting behavior, used to catch C++ regressions through shared JSONL events.

## Benchmarks

Measured with `mercury_latency` (one timed call per sample), Release build,
Clang on a Windows host (16×4700 MHz). Each path is **10,000** samples after
**500** warmup calls. Setup (seeding the book) is outside the timer. Times are
estimated nanoseconds from a cycle counter calibrated to `steady_clock`.

| Path | median (ns) | p99 (ns) |
| --- | ---: | ---: |
| Rest limit | 130 | 220 |
| Match 1-lot limit | 200 | 260 |
| Match deep book (32 levels) | 3070 | 5990 |
| Match one of 32 symbols | 250 | 490 |
| Iceberg tip-refill (hidden 128, display 1) | 3360 | 5400 |

Current measurements do **not** show a bottleneck that justifies adding symbol
sharding or multithreaded matching yet. Full table and methodology:
[`docs/latency.md`](docs/latency.md).

## Design choices / non-goals

- Single-threaded matching is intentional; correctness and measurement come first
- Integer ticks/lots avoid floating-point behavior in the hot path
- Symbol sharding is deferred until benches identify a concrete hotspot
- No production networking, FIX/WebSocket gateway, or UI
- No persistent database beyond JSONL event files
- No live-market connectivity or real-money trading
- No lock-free structures or multithreaded matching

Roadmap: [`docs/roadmap.md`](docs/roadmap.md).

## Build

Requires CMake 3.20+ and a C++23 compiler (GCC or Clang).

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Release latency benchmark:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target mercury_latency
./build-release/benchmarks/mercury_latency
```

## Python

Stdlib only. From `python/`:

```bash
python -m mercury_sim.generate -n 100 --seed 1 -o events.jsonl
python -m mercury_sim.generate -n 100 --mode market --seed 1 -o market.jsonl
python -m mercury_sim.analyze events.jsonl
python -m mercury_sim.replay events.jsonl
python -m mercury_sim.compare events.jsonl
python -m unittest discover -s tests
```

Optional C++ Engine bindings (pybind11):

```bash
cmake -S . -B build -DMERCURY_BUILD_PYTHON=ON
cmake --build build --target mercury_engine
# module lands in python/mercury_engine.*
python -c "import mercury_engine; e=mercury_engine.Engine(); print(e.snapshot())"
```

Replay the same JSONL with the C++ engine:

```bash
./build/apps/jsonl_replay events.jsonl
./build/apps/book_snapshot events.jsonl 5
./build/apps/account_report events.jsonl 1
./build/apps/account_report events.jsonl 1 mid
```
