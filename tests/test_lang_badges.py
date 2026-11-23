import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import badges  # noqa: E402


class BadgesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.alpha = np.zeros((32, 64), np.uint8)
        self.alpha[0:2, :] = 255
        self.alpha[10:20, 8:56] = 255
        self.result = badges.relabel(self.alpha, (6, 9, 58, 21), (8, 10, 56, 20), "НОВОЕ", badges.CONDENSED)
        self.ink = self.result[2:] > 128

    def test_the_frame_outside_the_erased_box_stays(self) -> None:
        self.assertTrue((self.result[0:2] == 255).all())

    def test_the_old_letters_are_erased(self) -> None:
        self.assertLess(self.ink.mean(), 0.5 * (self.alpha[2:] > 128).mean())

    def test_new_letters_take_the_rows_and_width_of_the_old_ones(self) -> None:
        rows = np.nonzero(self.ink.any(axis=1))[0] + 2
        columns = np.nonzero(self.ink.any(axis=0))[0]
        self.assertEqual((rows.min(), rows.max()), (10, 19))
        self.assertGreaterEqual(columns.min(), 8)
        self.assertLessEqual(columns.max(), 55)
        self.assertLessEqual(abs((columns.min() + columns.max()) / 2 - 31.5), 1.5)

    def test_the_erased_box_keeps_the_haze_of_the_original_background(self) -> None:
        hazy = self.alpha.copy()
        hazy[6:24, 4:60] = np.maximum(hazy[6:24, 4:60], 40)
        result = badges.relabel(hazy, (4, 6, 60, 24), (8, 10, 56, 20), "НОВОЕ", badges.CONDENSED)
        self.assertEqual(result[7, 30], 40)

    def test_a_glow_leaves_a_soft_edge_around_the_letters(self) -> None:
        glowing = badges.relabel(self.alpha, (6, 9, 58, 21), (8, 10, 56, 20), "НОВОЕ", badges.CONDENSED, glow=0.5)
        self.assertEqual(self.result[8].max(), 0)
        self.assertTrue(0 < glowing[8].max() < 128)


if __name__ == "__main__":
    unittest.main()
