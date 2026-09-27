#include "mercury/jsonl.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void write_levels(std::ostream& out, const std::vector<mercury::BookLevel>& levels) {
  out << '[';
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << "{\"price\":" << levels[i].price.ticks()
        << ",\"quantity\":" << levels[i].quantity.value()
        << ",\"order_count\":" << levels[i].order_count << '}';
  }
  out << ']';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "usage: book_snapshot <events.jsonl> [depth] [symbol]\n";
    return 1;
  }

  const std::size_t depth = (argc >= 3) ? static_cast<std::size_t>(std::stoul(argv[2])) : 10;
  const auto symbol = mercury::Symbol{
      (argc == 4) ? static_cast<std::uint64_t>(std::stoull(argv[3])) : 0};

  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "failed to open " << argv[1] << '\n';
    return 1;
  }

  const auto log = mercury::jsonl::load_event_log(input);
  mercury::Engine engine;
  mercury::replay(engine, log);
  const auto snap = engine.snapshot(depth, symbol);

  std::cout << "{\"bids\":";
  write_levels(std::cout, snap.bids);
  std::cout << ",\"asks\":";
  write_levels(std::cout, snap.asks);
  std::cout << "}\n";
  return 0;
}
