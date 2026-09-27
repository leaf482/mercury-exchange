#include "mercury/jsonl.hpp"
#include "mercury/snapshot.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: restart_snapshot <events.jsonl> [out.snap]\n";
    return 1;
  }

  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "failed to open " << argv[1] << '\n';
    return 1;
  }

  const auto log = mercury::jsonl::load_event_log(input);
  mercury::Engine engine;
  mercury::replay(engine, log);
  const std::string text =
      mercury::snapshot::format_restart(engine.restart_snapshot());

  if (argc == 3) {
    std::ofstream output(argv[2]);
    if (!output) {
      std::cerr << "failed to open " << argv[2] << '\n';
      return 1;
    }
    output << text;
    return 0;
  }

  std::cout << text;
  return 0;
}
