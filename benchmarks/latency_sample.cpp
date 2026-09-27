#include "mercury/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <x86intrin.h>

using mercury::AccountId;
using mercury::Engine;
using mercury::Order;
using mercury::OrderId;
using mercury::Price;
using mercury::Quantity;
using mercury::Side;
using mercury::Symbol;

namespace {

constexpr int kWarmup = 500;
constexpr int kSamples = 10'000;

double g_ns_per_tick = 1.0;

std::uint64_t ticks() {
  unsigned aux = 0;
  _mm_lfence();
  const auto t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

void calibrate() {
  const auto start = std::chrono::steady_clock::now();
  const auto t0 = ticks();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(200)) {
  }
  const auto t1 = ticks();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  g_ns_per_tick = static_cast<double>(nanos) / static_cast<double>(t1 - t0);
}

Order make_order(std::uint64_t id, Side side, Price price, std::uint64_t qty,
                 AccountId account, Symbol symbol = Symbol{0}) {
  return Order{
      .id = OrderId{id},
      .side = side,
      .price = price,
      .quantity = Quantity{qty},
      .account = account,
      .symbol = symbol,
  };
}

double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<std::size_t>(p * (values.size() - 1));
  return values[index];
}

// One timed call per sample. Setup stays outside the clock.
template <typename T>
void consume(T value) {
  volatile auto sink = value;
  (void)sink;
}

// One timed call per sample. Setup stays outside the clock.
template <typename Setup, typename Op>
void sample(const char* name, Setup setup, Op op) {
  for (int i = 0; i < kWarmup; ++i) {
    auto prepared = setup();
    consume(op(prepared));
  }

  std::vector<double> nanos;
  nanos.reserve(kSamples);
  for (int i = 0; i < kSamples; ++i) {
    auto prepared = setup();
    asm volatile("" ::: "memory");
    const auto start = ticks();
    auto result = op(prepared);
    const auto stop = ticks();
    asm volatile("" ::: "memory");
    consume(result);
    nanos.push_back(static_cast<double>(stop - start) * g_ns_per_tick);
  }

  std::cout << name << '\t' << static_cast<std::int64_t>(percentile(nanos, 0.50))
            << '\t' << static_cast<std::int64_t>(percentile(nanos, 0.95)) << '\t'
            << static_cast<std::int64_t>(percentile(nanos, 0.99)) << '\t' << kSamples
            << '\n';
}

struct RestInput {
  Engine engine;
  Order order;
};

struct MatchInput {
  Engine engine;
  Order buy;
};

struct CancelInput {
  Engine engine;
  OrderId id;
};

struct DeepInput {
  Engine engine;
  Order buy;
};

}  // namespace

int main() {
  calibrate();
  std::cout << "ns_per_tick\t" << g_ns_per_tick << '\n';
  std::cout << "path\tmedian_ns\tp95_ns\tp99_ns\tsamples\n";

  sample(
      "rest_limit",
      [] {
        static std::uint64_t id = 1;
        return RestInput{Engine{},
                         make_order(id++, Side::Buy, Price{100}, 1, AccountId{1})};
      },
      [](RestInput& in) {
        auto result = in.engine.add(std::move(in.order));
        return result.trades.size();
      });

  sample(
      "match_1_lot",
      [] {
        static std::uint64_t id = 1;
        MatchInput in{Engine{}, make_order(0, Side::Buy, Price{100}, 1, AccountId{2})};
        in.engine.add(make_order(id++, Side::Sell, Price{100}, 1, AccountId{1}));
        in.buy = make_order(id++, Side::Buy, Price{100}, 1, AccountId{2});
        return in;
      },
      [](MatchInput& in) {
        auto result = in.engine.add(std::move(in.buy));
        return result.trades.size();
      });

  sample(
      "cancel",
      [] {
        static std::uint64_t id = 1;
        CancelInput in{Engine{}, OrderId{id}};
        in.engine.add(make_order(id++, Side::Buy, Price{100}, 1, AccountId{1}));
        return in;
      },
      [](CancelInput& in) { return in.engine.cancel(in.id) ? 1u : 0u; });

  const int depths[] = {8, 32, 128};
  for (int levels : depths) {
    const std::string name = "deep_book_" + std::to_string(levels);
    sample(
        name.c_str(),
        [levels] {
          static std::uint64_t id = 1;
          DeepInput in{Engine{},
                       make_order(0, Side::Buy, Price{100}, 1, AccountId{2})};
          for (int level = 0; level < levels; ++level) {
            in.engine.add(make_order(id++, Side::Sell, Price{100 + level}, 1,
                                     AccountId{1}));
          }
          in.buy = make_order(id++, Side::Buy, Price{100 + levels - 1},
                              static_cast<std::uint64_t>(levels), AccountId{2});
          return in;
        },
        [](DeepInput& in) {
          auto result = in.engine.add(std::move(in.buy));
          return result.trades.size();
        });
  }

  const int symbol_counts[] = {1, 8, 32};
  for (int symbols : symbol_counts) {
    const std::string name = "match_symbols_" + std::to_string(symbols);
    sample(
        name.c_str(),
        [symbols] {
          static std::uint64_t id = 1;
          DeepInput in{Engine{},
                       make_order(0, Side::Buy, Price{100}, 1, AccountId{2})};
          for (int symbol = 0; symbol < symbols; ++symbol) {
            in.engine.add(make_order(id++, Side::Sell, Price{100}, 1, AccountId{1},
                                     Symbol{static_cast<std::uint64_t>(symbol)}));
          }
          in.buy = make_order(id++, Side::Buy, Price{100}, 1, AccountId{2}, Symbol{0});
          return in;
        },
        [](DeepInput& in) {
          auto result = in.engine.add(std::move(in.buy));
          return result.trades.size();
        });
  }

  const int mass_ns[] = {8, 32};
  for (int symbols : mass_ns) {
    const std::string name = "mass_cancel_" + std::to_string(symbols);
    sample(
        name.c_str(),
        [symbols] {
          static std::uint64_t id = 1;
          Engine engine;
          for (int symbol = 0; symbol < symbols; ++symbol) {
            engine.add(make_order(id++, Side::Buy, Price{100}, 1, AccountId{1},
                                  Symbol{static_cast<std::uint64_t>(symbol)}));
            engine.add(make_order(id++, Side::Buy, Price{99}, 1, AccountId{2},
                                  Symbol{static_cast<std::uint64_t>(symbol)}));
          }
          return engine;
        },
        [](Engine& engine) {
          return engine.mass_cancel(mercury::MassCancelFilter{
              .account = AccountId{1},
              .symbol = std::nullopt,
              .side = std::nullopt,
          });
        });
  }

  const int hidden_sizes[] = {32, 128};
  for (int hidden : hidden_sizes) {
    const std::string name = "iceberg_" + std::to_string(hidden);
    sample(
        name.c_str(),
        [hidden] {
          static std::uint64_t id = 1;
          DeepInput in{Engine{},
                       make_order(0, Side::Buy, Price{100}, 1, AccountId{2})};
          in.engine.add(Order{.id = OrderId{id++},
                              .side = Side::Sell,
                              .price = Price{100},
                              .quantity = Quantity{static_cast<std::uint64_t>(hidden)},
                              .account = AccountId{1},
                              .display = Quantity{1}});
          in.buy = make_order(id++, Side::Buy, Price{100},
                              static_cast<std::uint64_t>(hidden), AccountId{2});
          return in;
        },
        [](DeepInput& in) {
          auto result = in.engine.add(std::move(in.buy));
          return result.trades.size();
        });
  }

  sample(
      "account_report",
      [] {
        static std::uint64_t id = 1;
        Engine engine;
        engine.set_cash(AccountId{1}, 100'000);
        engine.add(make_order(id++, Side::Sell, Price{100}, 10, AccountId{2}));
        engine.add(make_order(id++, Side::Buy, Price{100}, 5, AccountId{1}));
        return engine;
      },
      [](Engine& engine) {
        auto report = engine.account_report(AccountId{1});
        return static_cast<std::uint64_t>(report.equity);
      });

  return 0;
}
