import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "console-test"))

import ab_table

PROFILE = """image 0x1, code up to 0x2
==== 0.0 s | mode: startup | game 0.0 fps | CPU total 0% (400% = 4 cores) | faults/s: x
     block 1 | label: -
==== 10.0 s | mode: normal | game 6.25 fps | CPU total 129% (400% = 4 cores) | faults/s: x
==== 10.0 s | mode: no draws | game 10.0 fps | CPU total 129% (400% = 4 cores) | faults/s: x
==== 10.0 s | mode: nothing | game 50.0 fps | CPU total 129% (400% = 4 cores) | faults/s: x
"""


class AbTableTest(unittest.TestCase):
    def test_blocks_keep_mode_and_fps_and_skip_startup(self) -> None:
        self.assertEqual(ab_table.parse_blocks(PROFILE), [("normal", 6.25), ("no draws", 10.0), ("nothing", 50.0)])

    def test_saving_is_measured_against_the_last_normal_block(self) -> None:
        rows = ab_table.lap_rows(ab_table.parse_blocks(PROFILE))
        self.assertEqual([r["mode"] for r in rows], ["normal", "no draws", "nothing"])
        self.assertAlmostEqual(rows[0]["frame_ms"], 160.0)
        self.assertIsNone(rows[0]["saved_ms"])
        self.assertAlmostEqual(rows[1]["saved_ms"], 60.0)
        self.assertAlmostEqual(rows[2]["saved_ms"], 140.0)

    def test_frozen_block_has_no_frame_time(self) -> None:
        rows = ab_table.lap_rows([("normal", 6.25), ("no draws", 0.0)])
        self.assertIsNone(rows[1]["frame_ms"])
        self.assertIsNone(rows[1]["saved_ms"])


if __name__ == "__main__":
    unittest.main()
