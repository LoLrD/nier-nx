import sys
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))

import title  # noqa: E402


class TitleTest(unittest.TestCase):
    def test_a_resized_sprite_keeps_its_center(self) -> None:
        self.assertEqual(title.centered((224.0, 184.0, 78.0, 26.0), 128, 30), (199.0, 182.0, 128.0, 30.0))

    def test_the_size_field_holds_width_and_height_minus_one(self) -> None:
        self.assertEqual(title.size_field(0x32036, 128, 30), (0x32036 & ~0x3FFFFFF) | 127 | (29 << 13))
        self.assertEqual(title.size_field(0xC0032036, 55, 26), 0xC0032036)

    def test_the_group_size_is_the_largest_that_fits_every_label(self) -> None:
        size = title.fit_size(lambda size: [size * 5, size * 7], [100, 100], start=80)
        self.assertEqual(size, 14)

    def test_line_centers_follow_the_original_rows(self) -> None:
        alpha = np.zeros((20, 8), np.uint8)
        alpha[2:5, 1] = 255
        alpha[10:15, 3] = 200
        alpha[17, 2] = 50
        self.assertEqual(title.line_centers(alpha), [3.5, 12.5])


if __name__ == "__main__":
    unittest.main()
