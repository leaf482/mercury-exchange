import unittest

from mercury_sim.engine import Engine
from mercury_sim.events import LimitEvent, StopEvent, TimeEvent
from mercury_sim.snapshot import engine_from_lines, lines_from_engine


class RestartSnapshotTests(unittest.TestCase):
    def test_resume_matches_live_engine(self) -> None:
        live = Engine(maker_bps=1, taker_bps=2, enforce_cash=True)
        live.set_cash(1, 100_000)
        live.set_cash(2, 100_000)
        live.apply(
            LimitEvent(id=1, side="buy", price=100, quantity=3, account=1)
        )
        live.apply(
            LimitEvent(
                id=2,
                side="sell",
                price=110,
                quantity=6,
                account=2,
                symbol=1,
                display=2,
            )
        )
        live.apply(
            LimitEvent(
                id=3, side="buy", price=110, quantity=1, account=1, symbol=1
            )
        )
        live.apply(
            StopEvent(
                id=4,
                side="buy",
                stop_price=140,
                quantity=1,
                account=1,
                symbol=1,
            )
        )
        live.apply(
            LimitEvent(
                id=5,
                side="sell",
                price=130,
                quantity=1,
                account=2,
                tif="gtd",
                expire_at=10,
            )
        )
        live.apply(TimeEvent(time=4))

        resumed = engine_from_lines(lines_from_engine(live))
        self.assertEqual(resumed.now(), 4)
        self.assertEqual(resumed.next_trade_id(), live.next_trade_id())
        self.assertEqual(resumed.cash(1), live.cash(1))
        self.assertEqual(resumed.reserved_cash(1), live.reserved_cash(1))
        self.assertEqual(resumed.pending_stop_count(1), 1)
        self.assertEqual(resumed.book(1).snapshot(4).asks[0].quantity, 1)

        take = LimitEvent(id=6, side="sell", price=100, quantity=1, account=2)
        self.assertEqual(live.apply(take), resumed.apply(take))
        self.assertEqual(live.advance_time(10), resumed.advance_time(10))
        self.assertIsNone(live.book(0).best_ask())
        self.assertIsNone(resumed.book(0).best_ask())


if __name__ == "__main__":
    unittest.main()
