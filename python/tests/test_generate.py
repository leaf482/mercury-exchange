import tempfile
import unittest
from pathlib import Path

from mercury_sim.analyze import summarize
from mercury_sim.events import (
    CancelEvent,
    LimitEvent,
    MarketEvent,
    MassCancelEvent,
    RejectEvent,
    ReplaceEvent,
    StopEvent,
    TimeEvent,
    event_from_dict,
    event_to_dict,
    read_jsonl,
    write_jsonl,
)
from mercury_sim.generate import generate_events, generate_stress_events


class GenerateTests(unittest.TestCase):
    def test_deterministic(self) -> None:
        a = generate_events(50, seed=7)
        b = generate_events(50, seed=7)
        self.assertEqual(a, b)

    def test_jsonl_roundtrip(self) -> None:
        events = generate_events(20, seed=1)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            write_jsonl(str(path), events)
            loaded = read_jsonl(str(path))
        self.assertEqual(events, loaded)

    def test_limit_dict_roundtrip(self) -> None:
        event = LimitEvent(id=1, side="buy", price=100, quantity=2, account=3)
        self.assertEqual(event, event_from_dict(event_to_dict(event)))

    def test_stress_is_deterministic_and_mixed(self) -> None:
        first = generate_stress_events(80, seed=3)
        second = generate_stress_events(80, seed=3)
        self.assertEqual(first, second)
        kinds = {type(event) for event in first}
        self.assertTrue(
            {
                LimitEvent,
                MarketEvent,
                CancelEvent,
                ReplaceEvent,
                MassCancelEvent,
                StopEvent,
                TimeEvent,
                RejectEvent,
            }
            <= kinds
        )

    def test_summarize_counts(self) -> None:
        events = generate_events(40, seed=2)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "events.jsonl"
            write_jsonl(str(path), events)
            summary = summarize(str(path))
        self.assertEqual(summary["events"], 40)
        self.assertGreater(sum(summary["types"].values()), 0)


if __name__ == "__main__":
    unittest.main()
