import tempfile
import unittest
from pathlib import Path

from mercury_sim.compare import compare_files, compare_invariants, find_app, find_jsonl_replay
from mercury_sim.events import write_jsonl
from mercury_sim.generate import generate_events, generate_stress_events

REPLAY_BIN = find_jsonl_replay()
BOOK_BIN = find_app("book_snapshot")
REPORT_BIN = find_app("account_report")
SNAPSHOT_BIN = find_app("restart_snapshot")


@unittest.skipUnless(REPLAY_BIN is not None, "jsonl_replay binary not built")
class ParityTests(unittest.TestCase):
    def test_python_and_cpp_trades_match(self) -> None:
        events = generate_events(120, seed=42)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            write_jsonl(str(path), events)
            py_trades, cxx_trades = compare_files(path, REPLAY_BIN)

        self.assertEqual(py_trades, cxx_trades)
        self.assertGreater(len(py_trades), 0)

    @unittest.skipUnless(
        REPLAY_BIN is not None
        and BOOK_BIN is not None
        and REPORT_BIN is not None
        and SNAPSHOT_BIN is not None,
        "replay tools not built",
    )
    def test_stress_invariants_match(self) -> None:
        for seed in (1, 7, 42):
            events = generate_stress_events(240, seed=seed)
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "events.jsonl"
                write_jsonl(str(path), events)
                summary = compare_invariants(
                    path, REPLAY_BIN, BOOK_BIN, REPORT_BIN, SNAPSHOT_BIN
                )
            self.assertGreater(summary["trades"], 0, msg=f"seed {seed} produced no trades")
            self.assertGreaterEqual(summary["books"], 1)


if __name__ == "__main__":
    unittest.main()
