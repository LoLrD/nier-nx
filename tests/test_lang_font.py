import struct
import sys
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "lang"))
sys.path.insert(0, str(ROOT / "tools" / "switch_prompts"))

import font  # noqa: E402
import xenos_dxt  # noqa: E402


class FontTest(unittest.TestCase):
    def test_glyphs_are_packed_in_rows_without_overlap(self) -> None:
        sizes = [(10, 20), (30, 25), (20, 10), (40, 12), (5, 5)]
        spots = font.pack(sizes, top=100, width=64, height=200)
        boxes = [(x, y, x + w, y + h) for (x, y), (w, h) in zip(spots, sizes)]
        for i, a in enumerate(boxes):
            self.assertGreaterEqual(a[1], 100)
            self.assertLessEqual(a[2], 64)
            for b in boxes[i + 1:]:
                self.assertTrue(a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], (a, b))

    def test_packing_past_the_atlas_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            font.pack([(60, 40), (60, 40)], top=0, width=64, height=70)

    def test_a_record_holds_the_box_metrics_page_and_proportional_mode(self) -> None:
        data = font.record(x=10, y=880, width=12, height=20, bearing_x=1, bearing_y=-19, advance=13, page=1)
        self.assertEqual(struct.unpack(">HHHHbbbbBBbB", data), (10, 880, 21, 899, 1, -19, 12, 0, 1, 13, 2, 1))

    def test_a_glyph_goes_into_one_channel_of_the_tiled_atlas(self) -> None:
        vram = bytearray(1024 * 1024 * 4)
        font.put_glyph(vram, page=1, x=33, y=40, image=np.array([[1, 2], [3, 4]], dtype=np.uint8))
        at = lambda x, y: xenos_dxt.tiled_offset(x, y, 1024, 2)
        self.assertEqual([vram[at(33, 40) + 2], vram[at(34, 40) + 2], vram[at(33, 41) + 2], vram[at(34, 41) + 2]],
                         [1, 2, 3, 4])
        self.assertEqual(sum(vram), 10)

    def test_cyrillic_sits_on_the_game_baseline(self) -> None:
        for char, last_row in (("Н", -3), ("х", -3)):
            image, _, bearing_y, _ = font.render_glyph(font.load_font(), ord(char))
            rows = np.where((image >= 128).any(axis=1))[0]
            self.assertEqual(bearing_y + rows.max(), last_row, char)
            self.assertGreaterEqual(image[0].max(), 8, char)

    def test_each_page_mask_writes_its_own_byte(self) -> None:
        at = xenos_dxt.tiled_offset(0, 0, 1024, 2)
        for page, byte in ((1, 2), (2, 1), (4, 0), (8, 3)):
            vram = bytearray(1024 * 1024 * 4)
            font.put_glyph(vram, page=page, x=0, y=0, image=np.array([[9]], dtype=np.uint8))
            self.assertEqual(vram[at:at + 4], bytes(9 if i == byte else 0 for i in range(4)), page)

    def test_a_cyrillic_capital_matches_the_latin_cap_height(self) -> None:
        image, bearing_x, bearing_y, advance = font.render_glyph(font.load_font(), ord("Н"))
        rows = np.where((image >= 128).any(axis=1))[0]
        self.assertIn(rows.max() - rows.min() + 1, range(20, 23))
        self.assertIn(bearing_y, range(-25, -22))
        self.assertIn(advance, range(20, 28))
        self.assertIn(bearing_x, range(-2, 3))


if __name__ == "__main__":
    unittest.main()
