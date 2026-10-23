# Mercury — C++ matching core (study branch)

`learn/cpp-core` is a **small** cut of the exchange simulator for learning C++.
It keeps only the matching skeleton: typed IDs, a price level, and a
price-time order book.

This is not the full Mercury project (no fees, cash, stops, JSONL, Python, or
benchmarks). The full engine lives on `main`.

## What to read (in order)

| File | Idea |
| --- | --- |
| `include/mercury/types.hpp` | Strong typedefs (`OrderId`, `Price`, `Quantity`) |
| `include/mercury/order.hpp` | Limit / market orders; GTC vs IOC |
| `include/mercury/trade.hpp` | Fill report; price = maker price |
| `include/mercury/price_level.hpp` | One price → FIFO `std::deque` |
| `include/mercury/order_book.hpp` | Bids/asks maps + match / cancel / market |

Suggested study path:

1. Why `Price` is an integer (ticks), not `double`
2. Why bids use `std::map<..., std::greater<>>` and asks use the default order
3. Walk `match_buy` / `fill_level`: maker price, partial fills, index cleanup
4. Compare GTC (rest leftover) vs IOC (discard leftover) vs `add_market`
5. Run the tests, then change something and watch a test fail

## Layout

```text
include/mercury/   header-only matching core
apps/demo.cpp      print limit / IOC / market fills
tests/             GoogleTest for rest / match / FIFO / cancel / IOC / market
```

## Build

Needs CMake 3.20+ and a C++23 compiler (Clang or GCC).

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++
cmake --build build
ctest --test-dir build --output-on-failure
./build/apps/demo
```

## Deliberately missing

FOK, GTD, iceberg, stops, STP, fees, cash, risk limits, multi-symbol Engine,
JSONL replay, Python twin, latency benches. Add those back on `main` when you
want the full portfolio project.
