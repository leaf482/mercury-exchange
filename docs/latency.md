# Latency notes

Published numbers come from `mercury_latency`, Release build, Clang, Windows
host (16 x 4700 MHz). One run, 2026-09-26.

Each path times **one call** per sample: **500** warmup calls, then **10,000**
samples. Book seeding stays outside the timer. The clock is `__rdtscp` (with
`lfence`), converted to nanoseconds with a 200 ms `steady_clock` calibration
(about **0.213 ns/tick** on this host). Percentile index is
`floor(p * (n - 1))` after sorting the samples.

Medians on a second run of the same binary stayed within a few percent.
p99 moved more, especially deep-book and iceberg tails, so treat the tail
columns as one snapshot.

| Path | median (ns) | p95 (ns) | p99 (ns) |
| --- | ---: | ---: | ---: |
| Rest limit | 130 | 160 | 220 |
| Match 1-lot limit | 200 | 220 | 260 |
| Cancel | 80 | 80 | 90 |
| Match deep book (8 levels) | 750 | 930 | 1500 |
| Match deep book (32 levels) | 3070 | 3460 | 5990 |
| Match deep book (128 levels) | 12690 | 27040 | 35281 |
| Match one of N symbols (1) | 200 | 230 | 370 |
| Match one of N symbols (8) | 200 | 230 | 380 |
| Match one of N symbols (32) | 250 | 320 | 490 |
| Mass cancel account × symbols (8) | 620 | 660 | 1130 |
| Mass cancel account × symbols (32) | 2860 | 3040 | 5230 |
| Iceberg tip-refill (hidden 32, display 1) | 1040 | 1080 | 1240 |
| Iceberg tip-refill (hidden 128, display 1) | 3360 | 5190 | 5400 |
| Account report | 90 | 110 | 130 |

Deep-book cases seed N ask levels (1 lot each) and sweep them with one buy.
Multi-symbol match seeds one resting ask per symbol (untimed) and crosses
symbol 0. Mass cancel rests two buys per symbol (accounts 1 and 2), then
cancels account 1. Iceberg cases rest one sell with `display=1` and take the
full hidden size (tip refill + requeue each lot).

`mercury_bench` (Google Benchmark, 20 repetitions) is still built. Its
repetition percentiles are a small sample of averaged runs, and `PauseTiming`
around sub-microsecond work distorted the multi-symbol path. Do not read those
columns as per-order latency. The table above replaces them.

## Shard decision

A single-thread match on one of 32 symbols is about **250 ns median** and
about **490 ns p99**. Going from 1 symbol to 32 added tens of nanoseconds, not
microseconds. The slow paths are book depth and hidden-size refill (deep 128
about **13 µs** median, about **35 µs** p99), which stay on one book. No
hotspot yet that justifies shard-by-symbol threading.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target mercury_latency
./build-release/benchmarks/mercury_latency
```
